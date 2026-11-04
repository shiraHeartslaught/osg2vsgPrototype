#include <ivevsg/CrsTransform.h>
#include <ivevsg/FileDialog.h>
#include <ivevsg/Hud.h>
#include <ivevsg/LayerOptions.h>
#include <ivevsg/LayerVisibility.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/ViewerApp.h>
#include <ivevsg/WorldView.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ivevsg
{
    namespace
    {
        /// Native CRS choices for files added from the panel. The first entry defers to the file.
        struct CrsChoice
        {
            const char* label;
            const char* crs; // "" = from the file (else the loader's default)
        };
        constexpr std::array<CrsChoice, 5> kCrsChoices = {{
            {"From file (else default)", ""},
            {"EPSG:4978  ECEF / geocentric", "EPSG:4978"},
            {"EPSG:3857  Web Mercator", "EPSG:3857"},
            {"EPSG:4326  WGS84 degrees", "EPSG:4326"},
            {"Custom...", nullptr},
        }};

        constexpr std::array<const char*, 5> kKindLabels = {"Auto (by extension)", "Terrain (DTED / DEM)", "Vector (shapefile, GeoJSON, ...)", "Raster imagery", "Scene / model"};
        constexpr std::array<LayerKind, 5> kKinds = {LayerKind::Auto, LayerKind::Terrain, LayerKind::Vector, LayerKind::Raster, LayerKind::Scene};
        constexpr std::array<const char*, 3> kInterpolationLabels = {"Nearest (stepped)", "Bilinear", "Bicubic (smooth)"};

        /// Text fields a layer's settings need between frames (ImGui edits them in place).
        struct LayerEdit
        {
            std::array<char, 64> scale{};
            std::array<char, 64> group{};
            bool initialised = false;
        };

        struct HudState
        {
            bool initialised = false;
            double longitude = 0.0;
            double latitude = 0.0;
            double zoom = 2.0;
            std::string lastViewKey;
            std::vector<std::string> viewNames; // one per worldViewKeys() entry
            int crsChoice = 0;
            int kindChoice = 0;
            std::array<char, 128> customCrs{};
            std::array<char, 1024> newLayerPath{};
            std::string dialogError;
            AsyncFileDialog dialog; // runs the system picker on a worker thread; polled every frame
            std::map<std::string, LayerEdit> edits; // by layer path
        };

        std::string chosenCrs(const HudState& state)
        {
            const CrsChoice& choice = kCrsChoices[static_cast<std::size_t>(std::clamp(state.crsChoice, 0, static_cast<int>(kCrsChoices.size()) - 1))];
            if (choice.crs) return choice.crs;
            return state.customCrs.data();
        }

        LayerSource newLayerSource(const HudState& state, const std::string& path)
        {
            LayerSource source;
            source.path = path;
            source.sourceCrs = chosenCrs(state);
            source.options.kind = kKinds[static_cast<std::size_t>(std::clamp(state.kindChoice, 0, static_cast<int>(kKinds.size()) - 1))];
            return source;
        }

        /// "native EPSG:4978 -> EPSG:3857" or "native EPSG:3857, shown as read"
        std::string crsSummary(const std::string& sourceCrs, const std::string& targetCrs)
        {
            std::string text = "native " + (sourceCrs.empty() ? std::string("n/a") : CrsTransform::displayName(sourceCrs));
            if (!targetCrs.empty())
                text += " -> " + CrsTransform::displayName(targetCrs);
            else
                text += ", shown as read";
            return text;
        }

        /// Why a layer is or is not on screen right now, in a few words ("" when nothing is in the way).
        std::string visibilityNote(const Layer& layer, const LayerVisibilityResult& result, double zoom, const VisibilitySettings& settings)
        {
            if (!layer.visible) return "off";
            if (!result.inZoomWindow)
            {
                const double floor = std::max(layer.source.options.minZoom, settings.globalMinZoom);
                char text[96];
                if (zoom < floor)
                    std::snprintf(text, sizeof(text), "hidden: shows from zoom %.1f%s", floor, settings.globalMinZoom > layer.source.options.minZoom ? " (global floor)" : "");
                else
                    std::snprintf(text, sizeof(text), "hidden: shows up to zoom %.1f", layer.source.options.maxZoom);
                return text;
            }
            if (result.replacedInGroup) return "hidden: another scale of '" + layer.effectiveLodGroup() + "' shows at this zoom";
            if (result.standingIn) return "beyond " + formatScaleDenominator(layer.effectiveScaleDenominator()) + ": no finer scale available, magnified";
            if (!result.resolved) return "beyond " + formatScaleDenominator(layer.effectiveScaleDenominator()) + ", magnified";
            return {};
        }

        void drawLayerSettings(ViewerApp& app, std::size_t index, LayerEdit& edit)
        {
            Layer& layer = app.layers()->layer(index);
            LayerOptions& o = layer.source.options;
            if (!edit.initialised)
            {
                std::snprintf(edit.scale.data(), edit.scale.size(), "%s", formatScaleDenominator(o.scaleDenominator).c_str());
                std::snprintf(edit.group.data(), edit.group.size(), "%s", o.lodGroup.c_str());
                edit.initialised = true;
            }
            bool needsReload = false;
            ImGui::TextDisabled("Appears between zoom levels (applies at once)");
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputDouble("min##zoom", &o.minZoom, 1.0, 1.0, "%.1f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputDouble("max##zoom", &o.maxZoom, 1.0, 1.0, "%.1f");
            o.minZoom = std::clamp(o.minZoom, 0.0, 30.0);
            o.maxZoom = std::clamp(o.maxZoom, o.minZoom, 30.0);

            ImGui::TextDisabled("Declared resolution and LOD group (applies at once)");
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputTextWithHint("scale", layer.declaredScale > 0.0 ? formatScaleDenominator(layer.declaredScale).c_str() : "1:50000000 or 50m", edit.scale.data(), edit.scale.size()))
            {
                const std::string text = edit.scale.data();
                o.scaleDenominator = text.empty() ? 0.0 : parseScaleDenominator(text).value_or(o.scaleDenominator);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::InputTextWithHint("group", lodGroupFromName(layer.source.path).c_str(), edit.group.data(), edit.group.size())) o.lodGroup = edit.group.data();
            if (o.scaleDenominator > 0.0 || layer.declaredScale > 0.0)
                ImGui::TextDisabled("resolves to zoom %.1f (+%.0f tolerance); group '%s'", zoomLevelForScaleDenominator(layer.effectiveScaleDenominator()), app.visibility().resolutionTolerance, layer.effectiveLodGroup().c_str());

            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputInt("detail level cap (-1 = own)", &o.detailLevelCap)) needsReload = true;
            o.detailLevelCap = std::max(-1, o.detailLevelCap);

            if (layer.kind == "vector")
            {
                float colour[4] = {o.vector.colour.r, o.vector.colour.g, o.vector.colour.b, o.vector.colour.a};
                if (ImGui::ColorEdit4("colour", colour, ImGuiColorEditFlags_NoInputs)) app.setLayerColour(index, vsg::vec4(colour[0], colour[1], colour[2], colour[3]));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                float width = o.vector.lineWidth;
                ImGui::BeginDisabled(!app.wideLinesSupported());
                if (ImGui::SliderFloat("width px", &width, 1.0f, 12.0f, "%.1f")) app.setLayerLineWidth(index, width);
                ImGui::EndDisabled();
                if (!app.wideLinesSupported() && ImGui::IsItemHovered()) ImGui::SetTooltip("This Vulkan device cannot draw lines wider than one pixel");
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::SliderFloat("marker px", &o.vector.pointSize, 1.0f, 32.0f, "%.0f")) needsReload = true;
            }
            else if (layer.kind == "terrain")
            {
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::SliderScalar("exaggeration", ImGuiDataType_Double, &o.terrain.exaggeration, [] { static const double v = 0.1; return &v; }(), [] { static const double v = 10.0; return &v; }(), "%.1f x")) needsReload = true;
                int interpolation = static_cast<int>(o.terrain.interpolation);
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::Combo("between posts", &interpolation, kInterpolationLabels.data(), static_cast<int>(kInterpolationLabels.size())))
                {
                    o.terrain.interpolation = static_cast<HeightInterpolation>(interpolation);
                    needsReload = true;
                }
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::InputInt("levels past the data", &o.terrain.oversampleLevels)) needsReload = true;
                o.terrain.oversampleLevels = std::clamp(o.terrain.oversampleLevels, 0, 12);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::InputInt("mesh cells / tile", &o.terrain.cellsPerTile, 8, 16)) needsReload = true;
                o.terrain.cellsPerTile = std::clamp(o.terrain.cellsPerTile, 2, 128);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::InputInt("texture px / tile", &o.terrain.texturePixels, 32, 64)) needsReload = true;
                o.terrain.texturePixels = std::clamp(o.terrain.texturePixels, 8, 512);
                if (ImGui::Checkbox("hillshade", &o.terrain.hillshade)) needsReload = true;
                if (layer.nativeLevel >= 0) ImGui::TextDisabled("full resolution at zoom %d, pages to %d (interpolated past %d)", layer.nativeLevel, layer.maxLevel, layer.nativeLevel);
            }
            if (needsReload) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Changes marked need a reload:");
            ImGui::SameLine(needsReload ? 0.0f : 0.0f);
            if (ImGui::SmallButton("Reload layer")) app.reloadLayer(index);
        }
    } // namespace

    std::function<bool()> createMapHud(MapHudContext context)
    {
        auto state = std::make_shared<HudState>();

        return [context, state]() -> bool {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin(context.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            if (!context.app)
            {
                ImGui::Text("no viewer");
                ImGui::End();
                return true;
            }
            ViewerApp& app = *context.app;
            // The view object is replaced when the projection switches, so take what the panel needs
            // from it as values and ask the app to switch between frames rather than switching here.
            WorldView& current = app.view();
            const std::string currentKey = current.key();
            const std::string currentName = current.name();
            if (state->lastViewKey != currentKey)
            {
                state->lastViewKey = currentKey;
                state->initialised = false; // a new view: re-read the go-to fields from it
            }

            // ---- display projection ----
            const std::vector<std::string> keys = worldViewKeys();
            if (state->viewNames.size() != keys.size())
            {
                state->viewNames.clear();
                for (const std::string& key : keys)
                {
                    std::unique_ptr<WorldView> candidate = createWorldView(key);
                    state->viewNames.push_back(candidate ? candidate->name() : key);
                }
            }
            ImGui::TextDisabled("Display projection");
            ImGui::SetNextItemWidth(-1.0f);
            const bool switching = app.viewSwitchPending();
            ImGui::BeginDisabled(switching);
            if (ImGui::BeginCombo("##display", switching ? "switching..." : currentName.c_str()))
            {
                for (std::size_t i = 0; i < keys.size(); ++i)
                {
                    const bool selected = keys[i] == currentKey;
                    if (ImGui::Selectable(state->viewNames[i].c_str(), selected) && !selected) app.requestViewSwitch(keys[i]);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (!app.lastSwitchError().empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", app.lastSwitchError().c_str());

            // ---- the view ----
            const ViewState vs = current.state();
            if (!state->initialised)
            {
                state->longitude = vs.centre.longitude;
                state->latitude = vs.centre.latitude;
                state->zoom = vs.zoomLevel;
                state->initialised = true;
            }
            ImGui::Text("Zoom %.2f   %.1f m/px", vs.zoomLevel, vs.metersPerPixel);
            ImGui::Text("OGC tile level %.2f   altitude %.0f m", context.scheme.levelForMetersPerPixel(std::max(vs.metersPerPixel, 1e-9)), vs.altitude);
            ImGui::Text("Centre  lon %9.4f  lat %8.4f", vs.centre.longitude, vs.centre.latitude);
            if (current.hasOrientation()) ImGui::Text("Heading %6.1f   tilt %5.1f", vs.heading, vs.tilt);
            const std::optional<GeoPoint> cursor = current.groundAtPointer();
            if (cursor)
                ImGui::Text("Cursor  lon %9.4f  lat %8.4f", cursor->longitude, cursor->latitude);
            else
                ImGui::Text("Cursor  (move the mouse over the ground)");
            if (app.elevation() && app.elevation()->hasTerrain())
            {
                const std::optional<double> underCursor = cursor ? app.terrainHeightAt(cursor->longitude, cursor->latitude) : std::nullopt;
                if (underCursor)
                    ImGui::Text("Terrain %.0f m under the cursor, %.0f m under the centre (camera rides above it)", *underCursor, current.surfaceHeight());
                else
                    ImGui::Text("Terrain %.0f m under the centre (camera rides above it)", current.surfaceHeight());
            }

            ImGui::Separator();
            ImGui::TextDisabled("Go to");
            ImGui::InputDouble("Longitude", &state->longitude, 0.1, 1.0, "%.4f");
            ImGui::InputDouble("Latitude", &state->latitude, 0.1, 1.0, "%.4f");
            const double minZoom = current.minZoomLevel();
            const double maxZoom = current.maxZoomLevel();
            ImGui::SliderScalar("Zoom level", ImGuiDataType_Double, &state->zoom, &minZoom, &maxZoom, "%.2f");
            if (ImGui::Button("Go"))
            {
                state->latitude = std::clamp(state->latitude, -89.99, 89.99);
                // the zoom level fixes the altitude through the viewport height: derive it the way the views do
                const double viewportHeight = app.camera() ? static_cast<double>(app.camera()->getViewport().height) : 1024.0;
                const double altitude = altitudeForZoomLevel(state->zoom, app.settings().fieldOfViewDegrees, viewportHeight, 40075016.685578488);
                app.lookAt(GeoPoint{state->longitude, state->latitude, 0.0}, altitude);
            }
            ImGui::SameLine();
            if (ImGui::Button("Home")) app.frameScene();
            if (current.hasOrientation())
            {
                ImGui::SameLine();
                if (ImGui::Button("North up")) current.setOrientation(0.0, 0.0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Use current view"))
            {
                state->longitude = vs.centre.longitude;
                state->latitude = vs.centre.latitude;
                state->zoom = vs.zoomLevel;
            }
            for (const std::string& line : current.controls()) ImGui::TextDisabled("%s", line.c_str());

            // ---- zoom limits and layer rules ----
            ImGui::Separator();
            ImGui::TextDisabled("Zoom limits and layer rules");
            const double zoomFloor = 0.0, zoomCeiling = 28.0;
            for (const std::string& key : keys)
            {
                double ceiling = app.viewMaxZoomLevel(key);
                const std::string label = (key == "globe" ? "Globe max zoom" : key == "mercator" ? "Map max zoom" : key + " max zoom") + (key == currentKey ? " (this view)" : "");
                if (ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &ceiling, &zoomFloor, &zoomCeiling, "%.1f")) app.setViewMaxZoomLevel(key, ceiling);
            }
            VisibilitySettings rules = app.visibility();
            bool rulesChanged = false;
            const double ruleFloor = 0.0, ruleCeiling = 24.0;
            rulesChanged |= ImGui::SliderScalar("Layers load from zoom", ImGuiDataType_Double, &rules.globalMinZoom, &ruleFloor, &ruleCeiling, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Below this zoom no layer is drawn or paged, whatever its own window says");
            rulesChanged |= ImGui::Checkbox("Drape vector layers over terrain", &rules.drape);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            rulesChanged |= ImGui::SliderScalar("from zoom", ImGuiDataType_Double, &rules.drapeMinZoom, &ruleFloor, &ruleCeiling, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vector tiles at or past this level put their vertices on the terrain (needs a terrain layer); changing it reloads the vector layers");
            const double tolFloor = 0.0, tolCeiling = 6.0;
            rulesChanged |= ImGui::SliderScalar("Resolution tolerance (zoom levels)", ImGuiDataType_Double, &rules.resolutionTolerance, &tolFloor, &tolCeiling, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far past its declared scale a layer still counts as resolving before a finer one in its group replaces it");
            if (rulesChanged) app.setVisibility(rules);

            // ---- layers ----
            ImGui::Separator();
            ImGui::TextDisabled("Layers  (native CRS -> display frame %s)", current.projection()->crs().c_str());
            if (std::shared_ptr<LayerStack> layersPtr = app.layers())
            {
                LayerStack& layers = *layersPtr;
                if (layers.hasBase())
                {
                    bool base = layers.baseVisible();
                    if (ImGui::Checkbox("Base map", &base)) layers.setBaseVisible(base);
                    const BaseInfo& info = app.baseInfo();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s, %s", info.kind.c_str(), crsSummary(info.sourceCrs, info.targetCrs).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.path.c_str());
                }
                else
                {
                    ImGui::TextDisabled("Base map: none for this projection");
                }
                const std::vector<LayerVisibilityResult>& verdicts = app.layerVisibility();
                std::size_t removeIndex = layers.layerCount(); // "none"
                for (std::size_t i = 0; i < layers.layerCount(); ++i)
                {
                    Layer& layer = layers.layer(i);
                    ImGui::PushID(static_cast<int>(i));
                    bool visible = layers.layerVisible(i);
                    if (ImGui::Checkbox(layer.name.c_str(), &visible)) layers.setLayerVisible(i, visible);
                    ImGui::SameLine();
                    ImGui::TextDisabled("[%s]", layer.kind.empty() ? "scene" : layer.kind.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Frame")) app.frameNode(layer.node);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) removeIndex = i;
                    ImGui::TextDisabled("    %s%s", crsSummary(layer.sourceCrs, layer.targetCrs).c_str(), layer.source.sourceCrs.empty() ? "" : " (as specified)");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", layer.source.path.c_str(), layer.info.c_str());
                    if (i < verdicts.size())
                    {
                        const std::string note = visibilityNote(layer, verdicts[i], vs.zoomLevel, app.visibility());
                        if (!note.empty()) ImGui::TextColored(layers.layerShown(i) ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "    %s", note.c_str());
                    }
                    if (ImGui::TreeNode("Settings"))
                    {
                        drawLayerSettings(app, i, state->edits[layer.source.path + "|" + layer.name]);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (removeIndex < layers.layerCount()) app.removeLayer(removeIndex);
            }

            // ---- add a layer ----
            // a picker finished on its worker thread? take its answer exactly once
            if (std::optional<FileDialogResult> picked = state->dialog.poll())
            {
                state->dialogError = picked->error;
                if (*picked)
                {
                    std::strncpy(state->newLayerPath.data(), picked->path.c_str(), state->newLayerPath.size() - 1);
                    state->newLayerPath.back() = '\0';
                    app.queueLayerLoad(newLayerSource(*state, picked->path)); // read, converted and compiled on a worker thread
                }
            }
            ImGui::TextDisabled("Add layer: how to read the file, and its native CRS");
            ImGui::SetNextItemWidth(220.0f);
            ImGui::Combo("##kind", &state->kindChoice, kKindLabels.data(), static_cast<int>(kKindLabels.size()));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##crs", kCrsChoices[static_cast<std::size_t>(state->crsChoice)].label))
            {
                for (std::size_t i = 0; i < kCrsChoices.size(); ++i)
                {
                    const bool selected = static_cast<int>(i) == state->crsChoice;
                    if (ImGui::Selectable(kCrsChoices[i].label, selected)) state->crsChoice = static_cast<int>(i);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!kCrsChoices[static_cast<std::size_t>(state->crsChoice)].crs)
            {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##customcrs", "EPSG:xxxx, PROJ string or WKT", state->customCrs.data(), state->customCrs.size());
            }
            else if (state->crsChoice == 0)
            {
                ImGui::TextDisabled("OSG files naming no CRS are taken as %s; shapefiles without .prj as EPSG:4326", app.settings().defaultSourceCrs.c_str());
            }
            // Browse... opens the system's file picker (via the desktop portal) without blocking rendering
            const bool choosing = state->dialog.busy();
            ImGui::BeginDisabled(choosing);
            if (ImGui::Button(choosing ? "Choosing..." : "Browse..."))
            {
                std::string startDirectory;
                if (state->newLayerPath[0] != '\0')
                {
                    std::error_code ec;
                    const std::filesystem::path typed(state->newLayerPath.data());
                    startDirectory = std::filesystem::is_directory(typed, ec) ? typed.string() : typed.parent_path().string();
                }
                state->dialog.open(sceneFileFilters(), startDirectory);
            }
            ImGui::SameLine();
            if (ImGui::Button(choosing ? "..." : "Folder..."))
            {
                std::string startDirectory;
                if (state->newLayerPath[0] != '\0')
                {
                    std::error_code ec;
                    const std::filesystem::path typed(state->newLayerPath.data());
                    startDirectory = std::filesystem::is_directory(typed, ec) ? typed.string() : typed.parent_path().string();
                }
                state->dialog.openFolder(startDirectory);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("A TMS / XYZ tile directory, or a directory of DTED cells (mosaicked as one terrain)");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-90.0f);
            ImGui::InputTextWithHint("##newlayer", "path: .ive / .vsgb / .tif / .dt2 / .shp / folder, optionally ?minzoom=3&scale=50m&colour=ff8800", state->newLayerPath.data(), state->newLayerPath.size());
            ImGui::SameLine();
            if (ImGui::Button("Add layer") && state->newLayerPath[0] != '\0')
            {
                app.queueLayerLoad(newLayerSource(*state, state->newLayerPath.data()));
            }
            for (const std::string& name : app.loadingLayers())
            {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Loading %s ...", name.c_str());
            }
            if (!state->dialogError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", state->dialogError.c_str());
            if (!app.lastLayerError().empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", app.lastLayerError().c_str());

            // ---- paging ----
            ImGui::Separator();
            ImGui::TextDisabled("Paging");
            if (std::shared_ptr<SceneLoader> loader = app.loader())
            {
                const LoadStats stats = loader->loadStats();
                ImGui::Text("Files read: %zu", stats.requests);
                ImGui::TextWrapped("Last: %s", stats.lastFile.c_str());
            }
            ImGui::Text("%.1f fps  (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));

            ImGui::End();
            return true;
        };
    }
} // namespace ivevsg
