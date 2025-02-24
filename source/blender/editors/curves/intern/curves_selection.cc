/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_array_utils.hh"
#include "BLI_index_mask_ops.hh"
#include "BLI_lasso_2d.h"
#include "BLI_rand.hh"
#include "BLI_rect.h"

#include "BKE_attribute.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"

#include "ED_curves.h"
#include "ED_object.h"
#include "ED_select_utils.h"
#include "ED_view3d.h"

namespace blender::ed::curves {

static IndexMask retrieve_selected_curves(const bke::CurvesGeometry &curves,
                                          Vector<int64_t> &r_indices)
{
  const IndexRange curves_range = curves.curves_range();
  const bke::AttributeAccessor attributes = curves.attributes();

  /* Interpolate from points to curves manually as a performance improvement, since we are only
   * interested in whether any point in each curve is selected. Retrieve meta data since
   * #lookup_or_default from the attribute API doesn't give the domain of the attribute. */
  std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(".selection");
  if (meta_data && meta_data->domain == ATTR_DOMAIN_POINT) {
    /* Avoid the interpolation from interpolating the attribute to the
     * curve domain by retrieving the point domain values directly. */
    const VArray<bool> selection = attributes.lookup_or_default<bool>(
        ".selection", ATTR_DOMAIN_POINT, true);
    if (selection.is_single()) {
      return selection.get_internal_single() ? IndexMask(curves_range) : IndexMask();
    }
    const OffsetIndices points_by_curve = curves.points_by_curve();
    return index_mask_ops::find_indices_based_on_predicate(
        curves_range, 512, r_indices, [&](const int64_t curve_i) {
          const IndexRange points = points_by_curve[curve_i];
          /* The curve is selected if any of its points are selected. */
          Array<bool, 32> point_selection(points.size());
          selection.materialize_compressed(points, point_selection);
          return point_selection.as_span().contains(true);
        });
  }
  const VArray<bool> selection = attributes.lookup_or_default<bool>(
      ".selection", ATTR_DOMAIN_CURVE, true);
  return index_mask_ops::find_indices_from_virtual_array(curves_range, selection, 2048, r_indices);
}

IndexMask retrieve_selected_curves(const Curves &curves_id, Vector<int64_t> &r_indices)
{
  const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  return retrieve_selected_curves(curves, r_indices);
}

IndexMask retrieve_selected_points(const bke::CurvesGeometry &curves, Vector<int64_t> &r_indices)
{
  return index_mask_ops::find_indices_from_virtual_array(
      curves.points_range(),
      curves.attributes().lookup_or_default<bool>(".selection", ATTR_DOMAIN_POINT, true),
      2048,
      r_indices);
}

IndexMask retrieve_selected_points(const Curves &curves_id, Vector<int64_t> &r_indices)
{
  const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  return retrieve_selected_points(curves, r_indices);
}

bke::GSpanAttributeWriter ensure_selection_attribute(bke::CurvesGeometry &curves,
                                                     const eAttrDomain selection_domain,
                                                     const eCustomDataType create_type)
{
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  if (attributes.contains(".selection")) {
    return attributes.lookup_for_write_span(".selection");
  }
  const int domain_size = attributes.domain_size(selection_domain);
  switch (create_type) {
    case CD_PROP_BOOL:
      attributes.add(".selection",
                     selection_domain,
                     CD_PROP_BOOL,
                     bke::AttributeInitVArray(VArray<bool>::ForSingle(true, domain_size)));
      break;
    case CD_PROP_FLOAT:
      attributes.add(".selection",
                     selection_domain,
                     CD_PROP_FLOAT,
                     bke::AttributeInitVArray(VArray<float>::ForSingle(1.0f, domain_size)));
      break;
    default:
      BLI_assert_unreachable();
  }
  return attributes.lookup_for_write_span(".selection");
}

void fill_selection_false(GMutableSpan selection)
{
  if (selection.type().is<bool>()) {
    selection.typed<bool>().fill(false);
  }
  else if (selection.type().is<float>()) {
    selection.typed<float>().fill(0.0f);
  }
}

void fill_selection_true(GMutableSpan selection)
{
  if (selection.type().is<bool>()) {
    selection.typed<bool>().fill(true);
  }
  else if (selection.type().is<float>()) {
    selection.typed<float>().fill(1.0f);
  }
}

static bool contains(const VArray<bool> &varray, const IndexRange range_to_check, const bool value)
{
  const CommonVArrayInfo info = varray.common_info();
  if (info.type == CommonVArrayInfo::Type::Single) {
    return *static_cast<const bool *>(info.data) == value;
  }
  if (info.type == CommonVArrayInfo::Type::Span) {
    const Span<bool> span(static_cast<const bool *>(info.data), varray.size());
    return threading::parallel_reduce(
        range_to_check,
        4096,
        false,
        [&](const IndexRange range, const bool init) {
          return init || span.slice(range).contains(value);
        },
        [&](const bool a, const bool b) { return a || b; });
  }
  return threading::parallel_reduce(
      range_to_check,
      2048,
      false,
      [&](const IndexRange range, const bool init) {
        if (init) {
          return init;
        }
        /* Alternatively, this could use #materialize to retrieve many values at once. */
        for (const int64_t i : range) {
          if (varray[i] == value) {
            return true;
          }
        }
        return false;
      },
      [&](const bool a, const bool b) { return a || b; });
}

bool has_anything_selected(const VArray<bool> &varray, const IndexRange range_to_check)
{
  return contains(varray, range_to_check, true);
}

bool has_anything_selected(const bke::CurvesGeometry &curves)
{
  const VArray<bool> selection = curves.attributes().lookup<bool>(".selection");
  return !selection || contains(selection, curves.curves_range(), true);
}

bool has_anything_selected(const GSpan selection)
{
  if (selection.type().is<bool>()) {
    return selection.typed<bool>().contains(true);
  }
  else if (selection.type().is<float>()) {
    for (const float elem : selection.typed<float>()) {
      if (elem > 0.0f) {
        return true;
      }
    }
  }
  return false;
}

static void invert_selection(MutableSpan<float> selection)
{
  threading::parallel_for(selection.index_range(), 2048, [&](IndexRange range) {
    for (const int i : range) {
      selection[i] = 1.0f - selection[i];
    }
  });
}

static void invert_selection(GMutableSpan selection)
{
  if (selection.type().is<bool>()) {
    array_utils::invert_booleans(selection.typed<bool>());
  }
  else if (selection.type().is<float>()) {
    invert_selection(selection.typed<float>());
  }
}

void select_all(bke::CurvesGeometry &curves, const eAttrDomain selection_domain, int action)
{
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  if (action == SEL_SELECT) {
    /* As an optimization, just remove the selection attributes when everything is selected. */
    attributes.remove(".selection");
  }
  else {
    bke::GSpanAttributeWriter selection = ensure_selection_attribute(
        curves, selection_domain, CD_PROP_BOOL);
    if (action == SEL_DESELECT) {
      fill_selection_false(selection.span);
    }
    else if (action == SEL_INVERT) {
      invert_selection(selection.span);
    }
    selection.finish();
  }
}

void select_ends(bke::CurvesGeometry &curves, int amount, bool end_points)
{
  const bool was_anything_selected = has_anything_selected(curves);
  const OffsetIndices points_by_curve = curves.points_by_curve();
  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, ATTR_DOMAIN_POINT, CD_PROP_BOOL);
  if (!was_anything_selected) {
    fill_selection_true(selection.span);
  }
  selection.span.type().to_static_type_tag<bool, float>([&](auto type_tag) {
    using T = typename decltype(type_tag)::type;
    if constexpr (std::is_void_v<T>) {
      BLI_assert_unreachable();
    }
    else {
      MutableSpan<T> selection_typed = selection.span.typed<T>();
      threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
        for (const int curve_i : range) {
          if (end_points) {
            selection_typed.slice(points_by_curve[curve_i].drop_back(amount)).fill(T(0));
          }
          else {
            selection_typed.slice(points_by_curve[curve_i].drop_front(amount)).fill(T(0));
          }
        }
      });
    }
  });
  selection.finish();
}

void select_linked(bke::CurvesGeometry &curves)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, ATTR_DOMAIN_POINT, CD_PROP_BOOL);

  threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
    for (const int curve_i : range) {
      GMutableSpan selection_curve = selection.span.slice(points_by_curve[curve_i]);
      if (has_anything_selected(selection_curve)) {
        fill_selection_true(selection_curve);
      }
    }
  });
  selection.finish();
}

void select_adjacent(bke::CurvesGeometry &curves, const bool deselect)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, ATTR_DOMAIN_POINT, CD_PROP_BOOL);
  const VArray<bool> cyclic = curves.cyclic();

  if (deselect) {
    invert_selection(selection.span);
  }

  if (selection.span.type().is<bool>()) {
    MutableSpan<bool> selection_typed = selection.span.typed<bool>();
    threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
      for (const int curve_i : range) {
        const IndexRange points = points_by_curve[curve_i];

        /* Handle all cases in the forward direction. */
        for (int point_i = points.first(); point_i < points.last(); point_i++) {
          if (!selection_typed[point_i] && selection_typed[point_i + 1]) {
            selection_typed[point_i] = true;
          }
        }

        /* Handle all cases in the backwards direction. */
        for (int point_i = points.last(); point_i > points.first(); point_i--) {
          if (!selection_typed[point_i] && selection_typed[point_i - 1]) {
            selection_typed[point_i] = true;
          }
        }

        /* Handle cyclic curve case. */
        if (cyclic[curve_i]) {
          if (selection_typed[points.first()] != selection_typed[points.last()]) {
            selection_typed[points.first()] = true;
            selection_typed[points.last()] = true;
          }
        }
      }
    });
  }
  else if (selection.span.type().is<float>()) {
    MutableSpan<float> selection_typed = selection.span.typed<float>();
    threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
      for (const int curve_i : range) {
        const IndexRange points = points_by_curve[curve_i];

        /* Handle all cases in the forward direction. */
        for (int point_i = points.first(); point_i < points.last(); point_i++) {
          if ((selection_typed[point_i] == 0.0f) && (selection_typed[point_i + 1] > 0.0f)) {
            selection_typed[point_i] = 1.0f;
          }
        }

        /* Handle all cases in the backwards direction. */
        for (int point_i = points.last(); point_i > points.first(); point_i--) {
          if ((selection_typed[point_i] == 0.0f) && (selection_typed[point_i - 1] > 0.0f)) {
            selection_typed[point_i] = 1.0f;
          }
        }

        /* Handle cyclic curve case. */
        if (cyclic[curve_i]) {
          if (selection_typed[points.first()] != selection_typed[points.last()]) {
            selection_typed[points.first()] = 1.0f;
            selection_typed[points.last()] = 1.0f;
          }
        }
      }
    });
  }

  if (deselect) {
    invert_selection(selection.span);
  }

  selection.finish();
}

void select_random(bke::CurvesGeometry &curves,
                   const eAttrDomain selection_domain,
                   uint32_t random_seed,
                   float probability)
{
  RandomNumberGenerator rng{random_seed};
  const auto next_bool_random_value = [&]() { return rng.get_float() <= probability; };

  const bool was_anything_selected = has_anything_selected(curves);
  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, selection_domain, CD_PROP_BOOL);
  if (!was_anything_selected) {
    curves::fill_selection_true(selection.span);
  }
  selection.span.type().to_static_type_tag<bool, float>([&](auto type_tag) {
    using T = typename decltype(type_tag)::type;
    if constexpr (std::is_void_v<T>) {
      BLI_assert_unreachable();
    }
    else {
      MutableSpan<T> selection_typed = selection.span.typed<T>();
      switch (selection_domain) {
        case ATTR_DOMAIN_POINT: {
          for (const int point_i : selection_typed.index_range()) {
            const bool random_value = next_bool_random_value();
            if (!random_value) {
              selection_typed[point_i] = T(0);
            }
          }

          break;
        }
        case ATTR_DOMAIN_CURVE: {
          for (const int curve_i : curves.curves_range()) {
            const bool random_value = next_bool_random_value();
            if (!random_value) {
              selection_typed[curve_i] = T(0);
            }
          }
          break;
        }
        default:
          BLI_assert_unreachable();
      }
    }
  });
  selection.finish();
}

static void apply_selection_operation_at_index(GMutableSpan selection,
                                               const int index,
                                               const eSelectOp sel_op)
{
  if (selection.type().is<bool>()) {
    MutableSpan<bool> selection_typed = selection.typed<bool>();
    switch (sel_op) {
      case SEL_OP_ADD:
      case SEL_OP_SET:
        selection_typed[index] = true;
        break;
      case SEL_OP_SUB:
        selection_typed[index] = false;
        break;
      case SEL_OP_XOR:
        selection_typed[index] = !selection_typed[index];
        break;
      default:
        break;
    }
  }
  else if (selection.type().is<float>()) {
    MutableSpan<float> selection_typed = selection.typed<float>();
    switch (sel_op) {
      case SEL_OP_ADD:
      case SEL_OP_SET:
        selection_typed[index] = 1.0f;
        break;
      case SEL_OP_SUB:
        selection_typed[index] = 0.0f;
        break;
      case SEL_OP_XOR:
        selection_typed[index] = 1.0f - selection_typed[index];
        break;
      default:
        break;
    }
  }
}

static bool find_closest_point_to_screen_co(const Depsgraph &depsgraph,
                                            const ARegion *region,
                                            const RegionView3D *rv3d,
                                            const Object &object,
                                            const bke::CurvesGeometry &curves,
                                            float2 mouse_pos,
                                            float radius,
                                            FindClosestData &closest_data)
{
  float4x4 projection;
  ED_view3d_ob_project_mat_get(rv3d, &object, projection.ptr());

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(depsgraph, object);

  const float radius_sq = pow2f(radius);
  closest_data = threading::parallel_reduce(
      curves.points_range(),
      1024,
      closest_data,
      [&](const IndexRange point_range, const FindClosestData &init) {
        FindClosestData best_match = init;
        for (const int point_i : point_range) {
          const float3 pos = deformation.positions[point_i];

          /* Find the position of the point in screen space. */
          float2 pos_proj;
          ED_view3d_project_float_v2_m4(region, pos, pos_proj, projection.ptr());

          const float distance_proj_sq = math::distance_squared(pos_proj, mouse_pos);
          if (distance_proj_sq > radius_sq ||
              distance_proj_sq > best_match.distance * best_match.distance) {
            /* Ignore the point because it's too far away or there is already a better point. */
            continue;
          }

          FindClosestData better_candidate;
          better_candidate.index = point_i;
          better_candidate.distance = std::sqrt(distance_proj_sq);

          best_match = better_candidate;
        }
        return best_match;
      },
      [](const FindClosestData &a, const FindClosestData &b) {
        if (a.distance < b.distance) {
          return a;
        }
        return b;
      });
  if (closest_data.index >= 0) {
    return true;
  }

  return false;
}

static bool find_closest_curve_to_screen_co(const Depsgraph &depsgraph,
                                            const ARegion *region,
                                            const RegionView3D *rv3d,
                                            const Object &object,
                                            const bke::CurvesGeometry &curves,
                                            float2 mouse_pos,
                                            float radius,
                                            FindClosestData &closest_data)
{
  float4x4 projection;
  ED_view3d_ob_project_mat_get(rv3d, &object, projection.ptr());

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(depsgraph, object);

  const float radius_sq = pow2f(radius);

  const OffsetIndices points_by_curve = curves.points_by_curve();
  closest_data = threading::parallel_reduce(
      curves.curves_range(),
      256,
      closest_data,
      [&](const IndexRange curves_range, const FindClosestData &init) {
        FindClosestData best_match = init;
        for (const int curve_i : curves_range) {
          if (points_by_curve.size(curve_i) == 1) {
            const float3 pos = deformation.positions[points_by_curve[curve_i].first()];

            /* Find the position of the point in screen space. */
            float2 pos_proj;
            ED_view3d_project_float_v2_m4(region, pos, pos_proj, projection.ptr());

            const float distance_proj_sq = math::distance_squared(pos_proj, mouse_pos);
            if (distance_proj_sq > radius_sq ||
                distance_proj_sq > best_match.distance * best_match.distance) {
              /* Ignore the point because it's too far away or there is already a better point.
               */
              continue;
            }

            FindClosestData better_candidate;
            better_candidate.index = curve_i;
            better_candidate.distance = std::sqrt(distance_proj_sq);

            best_match = better_candidate;
            continue;
          }

          for (const int segment_i : points_by_curve[curve_i].drop_back(1)) {
            const float3 pos1 = deformation.positions[segment_i];
            const float3 pos2 = deformation.positions[segment_i + 1];

            float2 pos1_proj, pos2_proj;
            ED_view3d_project_float_v2_m4(region, pos1, pos1_proj, projection.ptr());
            ED_view3d_project_float_v2_m4(region, pos2, pos2_proj, projection.ptr());

            const float distance_proj_sq = dist_squared_to_line_segment_v2(
                mouse_pos, pos1_proj, pos2_proj);
            if (distance_proj_sq > radius_sq ||
                distance_proj_sq > best_match.distance * best_match.distance) {
              /* Ignore the segment because it's too far away or there is already a better point.
               */
              continue;
            }

            FindClosestData better_candidate;
            better_candidate.index = curve_i;
            better_candidate.distance = std::sqrt(distance_proj_sq);

            best_match = better_candidate;
          }
        }
        return best_match;
      },
      [](const FindClosestData &a, const FindClosestData &b) {
        if (a.distance < b.distance) {
          return a;
        }
        return b;
      });

  if (closest_data.index >= 0) {
    return true;
  }

  return false;
}

bool select_pick(const ViewContext &vc,
                 const Object &object,
                 bke::CurvesGeometry &curves,
                 const eAttrDomain selection_domain,
                 const SelectPick_Params &params,
                 const int2 coord,
                 FindClosestData initial)
{
  FindClosestData closest = initial;

  bool found = false;
  if (selection_domain == ATTR_DOMAIN_POINT) {
    found = find_closest_point_to_screen_co(*vc.depsgraph,
                                            vc.region,
                                            vc.rv3d,
                                            object,
                                            curves,
                                            float2(coord),
                                            ED_view3d_select_dist_px(),
                                            closest);
  }
  else if (selection_domain == ATTR_DOMAIN_CURVE) {
    found = find_closest_curve_to_screen_co(*vc.depsgraph,
                                            vc.region,
                                            vc.rv3d,
                                            object,
                                            curves,
                                            float2(coord),
                                            ED_view3d_select_dist_px(),
                                            closest);
  }

  bool deselected = false;
  if (params.sel_op == SEL_OP_SET) {
    bke::GSpanAttributeWriter selection = ensure_selection_attribute(
        curves, selection_domain, CD_PROP_BOOL);
    if (found || ((params.deselect_all && has_anything_selected(selection.span)))) {
      fill_selection_false(selection.span);
      deselected = true;
    }
    selection.finish();
  }

  if (found) {
    bke::GSpanAttributeWriter selection = ensure_selection_attribute(
        curves, selection_domain, CD_PROP_BOOL);
    apply_selection_operation_at_index(selection.span, closest.index, params.sel_op);
    selection.finish();
  }

  return deselected || found;
}

bool select_box(const ViewContext &vc,
                bke::CurvesGeometry &curves,
                const eAttrDomain selection_domain,
                const rcti &rect,
                const eSelectOp sel_op)
{
  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, selection_domain, CD_PROP_BOOL);

  bool changed = false;
  if (sel_op == SEL_OP_SET) {
    fill_selection_false(selection.span);
    changed = true;
  }

  float4x4 projection;
  ED_view3d_ob_project_mat_get(vc.rv3d, vc.obact, projection.ptr());

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*vc.depsgraph, *vc.obact);
  const OffsetIndices points_by_curve = curves.points_by_curve();
  if (selection_domain == ATTR_DOMAIN_POINT) {
    threading::parallel_for(curves.points_range(), 1024, [&](const IndexRange point_range) {
      for (const int point_i : point_range) {
        float2 pos_proj;
        ED_view3d_project_float_v2_m4(
            vc.region, deformation.positions[point_i], pos_proj, projection.ptr());
        if (BLI_rcti_isect_pt_v(&rect, int2(pos_proj))) {
          apply_selection_operation_at_index(selection.span, point_i, sel_op);
          changed = true;
        }
      }
    });
  }
  else if (selection_domain == ATTR_DOMAIN_CURVE) {
    threading::parallel_for(curves.curves_range(), 512, [&](const IndexRange curves_range) {
      for (const int curve_i : curves_range) {
        if (points_by_curve.size(curve_i) == 1) {
          float2 pos_proj;
          ED_view3d_project_float_v2_m4(vc.region,
                                        deformation.positions[points_by_curve[curve_i].first()],
                                        pos_proj,
                                        projection.ptr());
          if (BLI_rcti_isect_pt_v(&rect, int2(pos_proj))) {
            apply_selection_operation_at_index(selection.span, curve_i, sel_op);
            changed = true;
          }
          continue;
        }
        for (const int segment_i : points_by_curve[curve_i].drop_back(1)) {
          const float3 pos1 = deformation.positions[segment_i];
          const float3 pos2 = deformation.positions[segment_i + 1];

          float2 pos1_proj, pos2_proj;
          ED_view3d_project_float_v2_m4(vc.region, pos1, pos1_proj, projection.ptr());
          ED_view3d_project_float_v2_m4(vc.region, pos2, pos2_proj, projection.ptr());

          if (BLI_rcti_isect_segment(&rect, int2(pos1_proj), int2(pos2_proj))) {
            apply_selection_operation_at_index(selection.span, curve_i, sel_op);
            changed = true;
            break;
          }
        }
      }
    });
  }
  selection.finish();

  return changed;
}

bool select_lasso(const ViewContext &vc,
                  bke::CurvesGeometry &curves,
                  const eAttrDomain selection_domain,
                  const Span<int2> coords,
                  const eSelectOp sel_op)
{
  rcti bbox;
  const int(*coord_array)[2] = reinterpret_cast<const int(*)[2]>(coords.data());
  BLI_lasso_boundbox(&bbox, coord_array, coords.size());

  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, selection_domain, CD_PROP_BOOL);

  bool changed = false;
  if (sel_op == SEL_OP_SET) {
    fill_selection_false(selection.span);
    changed = true;
  }

  float4x4 projection;
  ED_view3d_ob_project_mat_get(vc.rv3d, vc.obact, projection.ptr());

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*vc.depsgraph, *vc.obact);
  const OffsetIndices points_by_curve = curves.points_by_curve();
  if (selection_domain == ATTR_DOMAIN_POINT) {
    threading::parallel_for(curves.points_range(), 1024, [&](const IndexRange point_range) {
      for (const int point_i : point_range) {
        float2 pos_proj;
        ED_view3d_project_float_v2_m4(
            vc.region, deformation.positions[point_i], pos_proj, projection.ptr());
        /* Check the lasso bounding box first as an optimization. */
        if (BLI_rcti_isect_pt_v(&bbox, int2(pos_proj)) &&
            BLI_lasso_is_point_inside(
                coord_array, coords.size(), int(pos_proj.x), int(pos_proj.y), IS_CLIPPED)) {
          apply_selection_operation_at_index(selection.span, point_i, sel_op);
          changed = true;
        }
      }
    });
  }
  else if (selection_domain == ATTR_DOMAIN_CURVE) {
    threading::parallel_for(curves.curves_range(), 512, [&](const IndexRange curves_range) {
      for (const int curve_i : curves_range) {
        if (points_by_curve.size(curve_i) == 1) {
          float2 pos_proj;
          ED_view3d_project_float_v2_m4(vc.region,
                                        deformation.positions[points_by_curve[curve_i].first()],
                                        pos_proj,
                                        projection.ptr());
          /* Check the lasso bounding box first as an optimization. */
          if (BLI_rcti_isect_pt_v(&bbox, int2(pos_proj)) &&
              BLI_lasso_is_point_inside(
                  coord_array, coords.size(), int(pos_proj.x), int(pos_proj.y), IS_CLIPPED)) {
            apply_selection_operation_at_index(selection.span, curve_i, sel_op);
            changed = true;
          }
          continue;
        }
        for (const int segment_i : points_by_curve[curve_i].drop_back(1)) {
          const float3 pos1 = deformation.positions[segment_i];
          const float3 pos2 = deformation.positions[segment_i + 1];

          float2 pos1_proj, pos2_proj;
          ED_view3d_project_float_v2_m4(vc.region, pos1, pos1_proj, projection.ptr());
          ED_view3d_project_float_v2_m4(vc.region, pos2, pos2_proj, projection.ptr());

          /* Check the lasso bounding box first as an optimization. */
          if (BLI_rcti_isect_segment(&bbox, int2(pos1_proj), int2(pos2_proj)) &&
              BLI_lasso_is_edge_inside(coord_array,
                                       coords.size(),
                                       int(pos1_proj.x),
                                       int(pos1_proj.y),
                                       int(pos2_proj.x),
                                       int(pos2_proj.y),
                                       IS_CLIPPED)) {
            apply_selection_operation_at_index(selection.span, curve_i, sel_op);
            changed = true;
            break;
          }
        }
      }
    });
  }
  selection.finish();

  return changed;
}

bool select_circle(const ViewContext &vc,
                   bke::CurvesGeometry &curves,
                   const eAttrDomain selection_domain,
                   const int2 coord,
                   const float radius,
                   const eSelectOp sel_op)
{
  const float radius_sq = pow2f(radius);
  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, selection_domain, CD_PROP_BOOL);

  bool changed = false;
  if (sel_op == SEL_OP_SET) {
    fill_selection_false(selection.span);
    changed = true;
  }

  float4x4 projection;
  ED_view3d_ob_project_mat_get(vc.rv3d, vc.obact, projection.ptr());

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*vc.depsgraph, *vc.obact);
  const OffsetIndices points_by_curve = curves.points_by_curve();
  if (selection_domain == ATTR_DOMAIN_POINT) {
    threading::parallel_for(curves.points_range(), 1024, [&](const IndexRange point_range) {
      for (const int point_i : point_range) {
        float2 pos_proj;
        ED_view3d_project_float_v2_m4(
            vc.region, deformation.positions[point_i], pos_proj, projection.ptr());
        if (math::distance_squared(pos_proj, float2(coord)) <= radius_sq) {
          apply_selection_operation_at_index(selection.span, point_i, sel_op);
          changed = true;
        }
      }
    });
  }
  else if (selection_domain == ATTR_DOMAIN_CURVE) {
    threading::parallel_for(curves.curves_range(), 512, [&](const IndexRange curves_range) {
      for (const int curve_i : curves_range) {
        if (points_by_curve.size(curve_i) == 1) {
          float2 pos_proj;
          ED_view3d_project_float_v2_m4(vc.region,
                                        deformation.positions[points_by_curve[curve_i].first()],
                                        pos_proj,
                                        projection.ptr());
          if (math::distance_squared(pos_proj, float2(coord)) <= radius_sq) {
            apply_selection_operation_at_index(selection.span, curve_i, sel_op);
            changed = true;
          }
          continue;
        }
        for (const int segment_i : points_by_curve[curve_i].drop_back(1)) {
          const float3 pos1 = deformation.positions[segment_i];
          const float3 pos2 = deformation.positions[segment_i + 1];

          float2 pos1_proj, pos2_proj;
          ED_view3d_project_float_v2_m4(vc.region, pos1, pos1_proj, projection.ptr());
          ED_view3d_project_float_v2_m4(vc.region, pos2, pos2_proj, projection.ptr());

          const float distance_proj_sq = dist_squared_to_line_segment_v2(
              float2(coord), pos1_proj, pos2_proj);
          if (distance_proj_sq <= radius_sq) {
            apply_selection_operation_at_index(selection.span, curve_i, sel_op);
            changed = true;
            break;
          }
        }
      }
    });
  }
  selection.finish();

  return changed;
}

}  // namespace blender::ed::curves
