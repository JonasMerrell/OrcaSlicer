///|/ Staircase wave overhangs: the slicing pass. See WaveStaircase.hpp.
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#include "WaveStaircase.hpp"

#include <boost/log/trivial.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r::WaveOverhangs::Staircase {

bool is_band(const ExtrusionEntity *entity)
{
    const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity);
    if (coll == nullptr || !coll->no_sort || coll->entities.empty())
        return false;
    const auto *path = dynamic_cast<const ExtrusionPath *>(coll->entities.front());
    return path != nullptr && path->staircase_band;
}

namespace {

// Undo a previous run on this layer region: drop the band collections and put the wave paths
// back at the front of their island collections, where the perimeter generator left them.
void restore(LayerRegion &layerm)
{
    ExtrusionEntitiesPtr &ents = layerm.perimeters.entities;
    bool had_band = false;
    for (size_t i = 0; i < ents.size();) {
        if (is_band(ents[i])) {
            delete ents[i];
            ents.erase(ents.begin() + i);
            had_band = true;
        } else
            ++i;
    }
    if (had_band) {
        for (auto &[idx, paths] : layerm.wave_staircase_stash) {
            if (idx >= ents.size())
                continue;
            auto *island = dynamic_cast<ExtrusionEntityCollection *>(ents[idx]);
            if (island == nullptr)
                continue;
            ExtrusionEntitiesPtr restored;
            restored.reserve(paths.size() + island->entities.size());
            for (const ExtrusionPath &p : paths)
                restored.push_back(p.clone());
            restored.insert(restored.end(), island->entities.begin(), island->entities.end());
            island->entities = std::move(restored);
        }
    }
    // Without a band the perimeters were regenerated and the stash is stale.
    layerm.wave_staircase_stash.clear();
}

StepPattern pattern_of(WaveStaircasePattern p)
{
    switch (p) {
    case WaveStaircasePattern::Rectilinear: return StepPattern::Rectilinear;
    case WaveStaircasePattern::Hilbert:     return StepPattern::Hilbert;
    case WaveStaircasePattern::Concentric:  return StepPattern::Concentric;
    default:                                return StepPattern::Waves;
    }
}

Polygons fill_area(const LayerRegion &layerm)
{
    Polygons out;
    for (const Surface &s : layerm.fill_surfaces.surfaces)
        if (s.surface_type != stBottom && s.surface_type != stBottomBridge)
            polygons_append(out, to_polygons(s.expolygon));
    return union_(out);
}

// Material printed at height z by every other instance on the plate (other objects, and the
// other copies of this one), in this object's frame. Every instance of this object shares the
// band, so each pairing counts. It is printed on the same layer as the band, before or after it,
// so a step must keep clear of it as of this object's own later material.
Polygons other_instances(const PrintObject &self, double z, const BoundingBox &near)
{
    Polygons out;
    const Print *print = self.print();
    if (print == nullptr)
        return out;
    for (const PrintObject *obj : print->objects()) {
        for (const Layer *l : obj->layers()) {
            if (l->print_z - l->height > z + EPSILON || l->print_z < z - EPSILON)
                continue;
            for (const PrintInstance &mine : self.instances())
                for (const PrintInstance &theirs : obj->instances()) {
                    if (obj == &self && &mine == &theirs)
                        continue;
                    const Point delta = theirs.shift - mine.shift;
                    for (const ExPolygon &e : l->lslices) {
                        BoundingBox b = get_extents(e);
                        b.translate(delta);
                        if (!b.overlap(near))
                            continue;
                        Polygons p = to_polygons(e);
                        for (Polygon &g : p)
                            g.translate(delta);
                        polygons_append(out, std::move(p));
                    }
                }
        }
    }
    return out;
}

} // namespace

void apply(PrintObject &object)
{
    LayerPtrs &layers = object.layers();
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id) {
        const PrintRegionConfig &rc = object.printing_region(region_id).config();
        // Always undo first: the setting may have just been switched off.
        for (Layer *layer : layers)
            if (region_id < layer->regions().size() && layer->get_region(int(region_id)) != nullptr)
                restore(*layer->get_region(int(region_id)));
        if (!rc.wave_overhangs.value || !rc.wave_overhang_staircase.value || rc.wave_overhang_staircase_layers.value < 2)
            continue;

        Params params;
        params.layers         = rc.wave_overhang_staircase_layers.value;
        params.stagger        = rc.wave_overhang_staircase_stagger.value;
        params.rings_per_step = rc.wave_overhang_staircase_rings_per_step.value;
        params.ring_spacing   = rc.wave_overhang_line_spacing.value;
        params.pattern        = pattern_of(rc.wave_overhang_staircase_pattern.value);
        const double max_height = rc.wave_overhang_staircase_max_height.value;
        const bool   ironing    = rc.ironing_type.value != IroningType::NoIroning;

        size_t bands = 0, max_steps = 0;
        // In order of height: a band claims fill of the layers above before the next wave layer looks at them.
        for (size_t li = 1; li < layers.size(); ++li) {
            Layer *layer = layers[li];
            if (layer->wave_overhang_floor_polygons.empty())
                continue;
            LayerRegion *layerm = layer->get_region(int(region_id));
            if (layerm == nullptr)
                continue;
            // Upper layers within the height cap.
            std::vector<size_t> upper;
            for (int k = 1; k < params.layers && li + size_t(k) < layers.size(); ++k) {
                const double dz = layers[li + k]->print_z - layer->print_z;
                if (dz > max_height + EPSILON)
                    break;
                upper.push_back(li + size_t(k));
            }
            if (upper.empty())
                continue;
            const Polygons floor_polys = union_(layer->wave_overhang_floor_polygons);
            const Layer   *lower       = layers[li - 1];

            ExtrusionEntitiesPtr &ents = layerm->perimeters.entities;
            const size_t n_islands = ents.size();
            for (size_t ci = 0; ci < n_islands; ++ci) {
                auto *island_coll = dynamic_cast<ExtrusionEntityCollection *>(ents[ci]);
                if (island_coll == nullptr || is_band(island_coll))
                    continue;
                BandInput in;
                for (const ExtrusionEntity *e : island_coll->entities)
                    if (const auto *p = dynamic_cast<const ExtrusionPath *>(e); p != nullptr && p->wave_overhang)
                        in.waves.push_back(*p);
                if (in.waves.empty())
                    continue;
                // The island the waves belong to.
                const Point probe = in.waves.front().first_point();
                int island_idx = -1;
                for (size_t s = 0; s < layer->lslices.size() && island_idx < 0; ++s)
                    if (layer->lslices_bboxes.size() > s && layer->lslices_bboxes[s].contains(probe) && layer->lslices[s].contains(probe))
                        island_idx = int(s);
                if (island_idx < 0)
                    for (size_t s = 0; s < layer->lslices.size() && island_idx < 0; ++s)
                        for (const Polygon &g : expand(ExPolygons{ layer->lslices[s] }, float(scale_(1.))))
                            if (g.contains(probe))
                                island_idx = int(s);
                if (island_idx < 0)
                    continue;
                in.island = to_polygons(layer->lslices[size_t(island_idx)]);
                in.footprint = intersection(floor_polys, expand(in.island, float(scale_(0.05))));
                if (in.footprint.empty())
                    continue;
                BoundingBox bb = get_extents(in.island);
                bb.offset(scale_(20.));
                for (const ExPolygon &e : lower->lslices)
                    if (get_extents(e).overlap(bb))
                        polygons_append(in.anchor, to_polygons(e));
                for (size_t s = 0; s < layer->lslices.size(); ++s)
                    if (int(s) != island_idx && layer->lslices_bboxes.size() > s && layer->lslices_bboxes[s].overlap(bb))
                        polygons_append(in.rest0, to_polygons(layer->lslices[s]));
                polygons_append(in.rest0, other_instances(object, layer->print_z, bb));
                if (ironing)
                    for (const Surface &s : layerm->fill_surfaces.surfaces)
                        if (s.surface_type == stTop)
                            polygons_append(in.rest0, to_polygons(s.expolygon));
                for (size_t k = 0; k < upper.size(); ++k) {
                    const Layer *ul  = layers[upper[k]];
                    const LayerRegion *ulm = ul->get_region(int(region_id));
                    if (ulm == nullptr)
                        break;
                    StepLayer sl;
                    sl.dz     = ul->print_z - layer->print_z;
                    sl.fill   = fill_area(*ulm);
                    sl.slices = to_polygons(ul->lslices);
                    polygons_append(sl.slices, other_instances(object, ul->print_z, bb));
                    sl.flow   = ulm->flow(frSolidInfill);
                    sl.angle  = float(Geometry::deg2rad(rc.solid_infill_direction.value + 90.));
                    in.steps.push_back(std::move(sl));
                }
                BandResult band = build_band(in, params);
                if (band.paths.empty())
                    continue;

                // The band now prints these areas of the layers above.
                for (size_t k = 0; k < band.step_regions.size(); ++k) {
                    LayerRegion *ulm = layers[upper[k]]->get_region(int(region_id));
                    const Polygons &cut = band.step_regions[k];
                    Surfaces kept;
                    for (const Surface &s : ulm->fill_surfaces.surfaces)
                        for (ExPolygon &e : diff_ex(to_polygons(s.expolygon), cut))
                            kept.emplace_back(s, std::move(e));
                    ulm->fill_surfaces.surfaces = std::move(kept);
                }
                // Move the island's wave paths into the band.
                ExtrusionEntitiesPtr remaining;
                ExtrusionPaths       stashed;
                for (ExtrusionEntity *e : island_coll->entities) {
                    if (auto *p = dynamic_cast<ExtrusionPath *>(e); p != nullptr && p->wave_overhang) {
                        stashed.push_back(*p);
                        delete e;
                    } else
                        remaining.push_back(e);
                }
                island_coll->entities = std::move(remaining);
                layerm->wave_staircase_stash.emplace_back(ci, std::move(stashed));
                auto *band_coll    = new ExtrusionEntityCollection();
                band_coll->no_sort = true;
                band_coll->append(band.paths);
                ents.push_back(band_coll);
                ++bands;
                max_steps = std::max(max_steps, band.step_regions.size());
            }
        }
        if (bands > 0)
            BOOST_LOG_TRIVIAL(info) << "Wave staircase: region " << region_id << ", " << bands << " bands, up to "
                                    << (max_steps + 1) << " layers each";
    }
}

} // namespace Slic3r::WaveOverhangs::Staircase
