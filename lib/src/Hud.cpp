#include <ivevsg/CrsTransform.h>
#include <ivevsg/FileDialog.h>
#include <ivevsg/Hud.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/ViewerApp.h>
#include <ivevsg/WorldView.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
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

        struct HudState
        {
            bool initialised = false;
            double longitude = 0.0;
            double latitude = 0.0;
            double zoom = 2.0;
            double maxZoom = 22.0;
            int viewChoice = -1;
            int crsChoice = 0;
            std::array<char, 128> customCrs{};
            std::array<char, 1024> newLayerPath{};
            std::string dialogError;
            std::string switchError;
            AsyncFileDialog dialog; // runs the system picker on a worker thread; polled every frame
        };

        std::string chosenCrs(const HudState& state)
        {
            const CrsChoice& choice = kCrsChoices[static_cast<std::size_t>(std::clamp(state.crsChoice, 0, static_cast<int>(kCrsChoices.size()) - 1))];
            if (choice.crs) return choice.crs;
            return state.customCrs.data();
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
    } // namespace

    std::function<bool()> createMapHud(MapHudContext context)
    {
        auto state = std::make_shared<HudState>();

        return [context, state]() -> bool {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin(context.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            if (!context.app)
            {
                ImGui::Text("no viewer");
                ImGui::End();
                return true;
            }
            ViewerApp& app = *context.app;
            WorldView& view = app.view();

            // ---- display projection ----
            const std::vector<std::string> keys = worldViewKeys();
            if (state->viewChoice < 0)
            {
                for (std::size_t i = 0; i < keys.size(); ++i)
                    if (keys[i] == view.key()) state->viewChoice = static_cast<int>(i);
            }
            ImGui::TextDisabled("Display projection");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##display", view.name().c_str()))
            {
                for (std::size_t i = 0; i < keys.size(); ++i)
                {
                    std::unique_ptr<WorldView> candidate = createWorldView(keys[i]);
                    const std::string label = candidate ? candidate->name() : keys[i];
                    const bool selected = keys[i] == view.key();
                    if (ImGui::Selectable(label.c_str(), selected) && !selected)
                    {
                        state->switchError.clear();
                        if (app.switchView(keys[i]))
                        {
                            state->viewChoice = static_cast<int>(i);
                            state->initialised = false; // re-read the go-to fields from the new view
                        }
                        else
                        {
                            state->switchError = app.error();
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!state->switchError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", state->switchError.c_str());

            // ---- the view ----
            WorldView& current = app.view(); // may have just changed
            const ViewState vs = current.state();
            if (!state->initialised)
            {
                state->longitude = vs.centre.longitude;
                state->latitude = vs.centre.latitude;
                state->zoom = vs.zoomLevel;
                state->maxZoom = current.maxZoomLevel();
                state->initialised = true;
            }
            ImGui::Text("Zoom %.2f   %.1f m/px", vs.zoomLevel, vs.metersPerPixel);
            ImGui::Text("OGC tile level %.2f   altitude %.0f m", context.scheme.levelForMetersPerPixel(std::max(vs.metersPerPixel, 1e-9)), vs.altitude);
            ImGui::Text("Centre  lon %9.4f  lat %8.4f", vs.centre.longitude, vs.centre.latitude);
            if (current.hasOrientation()) ImGui::Text("Heading %6.1f   tilt %5.1f", vs.heading, vs.tilt);
            if (std::optional<GeoPoint> cursor = current.groundAtPointer())
                ImGui::Text("Cursor  lon %9.4f  lat %8.4f", cursor->longitude, cursor->latitude);
            else
                ImGui::Text("Cursor  (move the mouse over the ground)");

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
            const double zoomFloor = 0.0, zoomCeiling = 28.0;
            if (ImGui::SliderScalar("Max zoom", ImGuiDataType_Double, &state->maxZoom, &zoomFloor, &zoomCeiling, "%.0f"))
            {
                current.setZoomLevelRange(current.minZoomLevel(), state->maxZoom);
            }
            for (const std::string& line : current.controls()) ImGui::TextDisabled("%s", line.c_str());

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
                std::size_t removeIndex = layers.layerCount(); // "none"
                for (std::size_t i = 0; i < layers.layerCount(); ++i)
                {
                    const Layer& layer = layers.layer(i);
                    ImGui::PushID(static_cast<int>(i));
                    bool visible = layers.layerVisible(i);
                    if (ImGui::Checkbox(layer.name.c_str(), &visible)) layers.setLayerVisible(i, visible);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Frame")) app.frameNode(layer.node);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) removeIndex = i;
                    ImGui::TextDisabled("    %s%s", crsSummary(layer.sourceCrs, layer.targetCrs).c_str(),
                                        layer.source.sourceCrs.empty() ? "" : " (as specified)");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layer.source.path.c_str());
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
                    app.queueLayerLoad(LayerSource{picked->path, "", chosenCrs(*state)}); // read, converted and compiled on a worker thread
                }
            }
            ImGui::TextDisabled("Add layer: native CRS of the file");
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
                ImGui::TextDisabled("Files naming no CRS are taken as %s", app.settings().defaultSourceCrs.c_str());
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
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-90.0f);
            ImGui::InputTextWithHint("##newlayer", "or type a path to .ive / .vsgb / .osgb ...", state->newLayerPath.data(), state->newLayerPath.size());
            ImGui::SameLine();
            if (ImGui::Button("Add layer") && state->newLayerPath[0] != '\0')
            {
                app.queueLayerLoad(LayerSource{state->newLayerPath.data(), "", chosenCrs(*state)});
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
