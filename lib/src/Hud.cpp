#include <ivevsg/FileDialog.h>
#include <ivevsg/Hud.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/ViewerApp.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace ivevsg
{
    namespace
    {
        double MercatorProjectionMaxLatitude() { return MercatorProjection::kMaxLatitude; }

        struct HudState
        {
            bool initialised = false;
            double longitude = 0.0;
            double latitude = 0.0;
            double zoom = 2.0;
            double maxZoom = 22.0;
            std::array<char, 1024> newLayerPath{};
            std::string dialogError;
            AsyncFileDialog dialog; // runs the system picker on a worker thread; polled every frame
        };
    } // namespace

    std::function<bool()> createMapHud(MapHudContext context)
    {
        auto state = std::make_shared<HudState>();

        return [context, state]() -> bool {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin(context.title.c_str());

            vsg::ref_ptr<MapNavigationHandler> navigation = context.app ? context.app->mapNavigation() : vsg::ref_ptr<MapNavigationHandler>();
            if (navigation && context.projection)
            {
                const MapNavigationSettings& nav = navigation->settings();
                const MapCamera camera = navigation->mapCamera();
                const double viewportWidth = navigation->viewportWidth();
                const double viewportHeight = navigation->viewportHeight();
                const double zoom = navigation->zoomLevel();
                const double mpp = metersPerPixel(camera.altitude, nav.fieldOfViewDegrees, viewportHeight);
                const GeoPoint centre = context.projection->worldToGeo(vsg::dvec3(camera.centre.x, camera.centre.y, 0.0));

                if (!state->initialised)
                {
                    state->longitude = centre.longitude;
                    state->latitude = centre.latitude;
                    state->zoom = zoom;
                    state->maxZoom = navigation->maxZoomLevel();
                    state->initialised = true;
                }

                ImGui::Text("Zoom %.2f   %.1f m/px", zoom, mpp);
                ImGui::Text("OGC tile level %.2f   altitude %.0f m", context.scheme.levelForMetersPerPixel(mpp), camera.altitude);
                ImGui::Text("Centre  lon %9.4f  lat %8.4f", centre.longitude, centre.latitude);
                const MapNavigationHandler::PointerPosition pointer = navigation->lastPointer();
                if (pointer.valid && viewportHeight > 0.0)
                {
                    const vsg::dvec2 ground = groundPointAtPixel(camera, pointer.x, pointer.y, viewportWidth, viewportHeight, nav.fieldOfViewDegrees);
                    const GeoPoint cursor = context.projection->worldToGeo(vsg::dvec3(ground.x, ground.y, 0.0));
                    ImGui::Text("Cursor  lon %9.4f  lat %8.4f", cursor.longitude, cursor.latitude);
                }
                else
                {
                    ImGui::Text("Cursor  (move the mouse over the map)");
                }

                ImGui::Separator();
                ImGui::TextDisabled("Go to");
                ImGui::InputDouble("Longitude", &state->longitude, 0.1, 1.0, "%.4f");
                ImGui::InputDouble("Latitude", &state->latitude, 0.1, 1.0, "%.4f");
                const double minZoom = navigation->minZoomLevel();
                const double maxZoom = navigation->maxZoomLevel();
                ImGui::SliderScalar("Zoom level", ImGuiDataType_Double, &state->zoom, &minZoom, &maxZoom, "%.2f");
                if (ImGui::Button("Go"))
                {
                    state->latitude = std::clamp(state->latitude, -MercatorProjectionMaxLatitude(), MercatorProjectionMaxLatitude());
                    const double altitude = altitudeForZoomLevel(state->zoom, nav.fieldOfViewDegrees, viewportHeight, nav.worldWidthMetres);
                    context.app->lookAt(GeoPoint{state->longitude, state->latitude, 0.0}, altitude);
                }
                ImGui::SameLine();
                if (ImGui::Button("Home")) context.app->frameScene();
                ImGui::SameLine();
                if (ImGui::Button("Use current view"))
                {
                    state->longitude = centre.longitude;
                    state->latitude = centre.latitude;
                    state->zoom = zoom;
                }

                const double zoomFloor = 0.0, zoomCeiling = 28.0;
                if (ImGui::SliderScalar("Max zoom", ImGuiDataType_Double, &state->maxZoom, &zoomFloor, &zoomCeiling, "%.0f"))
                {
                    navigation->setZoomLevelRange(navigation->minZoomLevel(), state->maxZoom);
                }
            }
            else
            {
                ImGui::Text("Trackball mode: no map camera");
            }

            ImGui::Separator();
            ImGui::TextDisabled("Layers");
            if (context.layers)
            {
                LayerStack& layers = *context.layers;
                if (layers.hasBase())
                {
                    bool base = layers.baseVisible();
                    if (ImGui::Checkbox("Base map", &base)) layers.setBaseVisible(base);
                }
                std::size_t removeIndex = layers.layerCount(); // "none"
                for (std::size_t i = 0; i < layers.layerCount(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    bool visible = layers.layerVisible(i);
                    if (ImGui::Checkbox(layers.layer(i).name.c_str(), &visible)) layers.setLayerVisible(i, visible);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Frame") && context.app) context.app->frameNode(layers.layer(i).node);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) removeIndex = i;
                    ImGui::PopID();
                }
                if (removeIndex < layers.layerCount()) layers.removeLayer(removeIndex);

                // a picker finished on its worker thread? take its answer exactly once
                if (std::optional<FileDialogResult> picked = state->dialog.poll())
                {
                    state->dialogError = picked->error;
                    if (*picked && context.app)
                    {
                        std::strncpy(state->newLayerPath.data(), picked->path.c_str(), state->newLayerPath.size() - 1);
                        state->newLayerPath.back() = '\0';
                        context.app->queueLayerLoad(picked->path); // read, converted and compiled on a worker thread
                    }
                }
                // Browse... opens the system's file picker (via the desktop portal) without blocking rendering
                const bool choosing = state->dialog.busy();
                ImGui::BeginDisabled(choosing);
                if (ImGui::Button(choosing ? "Choosing..." : "Browse...") && context.app)
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
                if (ImGui::Button("Add layer") && context.app && state->newLayerPath[0] != '\0')
                {
                    context.app->queueLayerLoad(state->newLayerPath.data());
                }
                if (context.app)
                {
                    for (const std::string& name : context.app->loadingLayers())
                    {
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Loading %s ...", name.c_str());
                    }
                }
                if (!state->dialogError.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", state->dialogError.c_str());
                }
                if (context.app && !context.app->lastLayerError().empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", context.app->lastLayerError().c_str());
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("Paging");
            if (context.loader)
            {
                const LoadStats stats = context.loader->loadStats();
                ImGui::Text("Files read: %zu", stats.requests);
                ImGui::TextWrapped("Last: %s", stats.lastFile.c_str());
            }
            ImGui::Text("%.1f fps  (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));

            ImGui::End();
            return true;
        };
    }
} // namespace ivevsg
