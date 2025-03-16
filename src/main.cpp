#include <glad/glad.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include "graphics/draw_info/draw_info.hpp"
#include "input/glfw_lambda_callback_manager/glfw_lambda_callback_manager.hpp"
#include "input/input_state/input_state.hpp"

#include "utility/texture_packer_model_loading/texture_packer_model_loading.hpp"
#include "utility/fixed_frequency_loop/fixed_frequency_loop.hpp"
#include "utility/resource_path/resource_path.hpp"

#include "graphics/rigged_model_loading/rigged_model_loading.hpp"
#include "graphics/vertex_geometry/vertex_geometry.hpp"
#include "graphics/shader_standard/shader_standard.hpp"
#include "graphics/texture_packer/texture_packer.hpp"
#include "graphics/batcher/generated/batcher.hpp"
#include "graphics/shader_cache/shader_cache.hpp"
#include "graphics/fps_camera/fps_camera.hpp"
#include "graphics/window/window.hpp"
#include "graphics/colors/colors.hpp"

#include "utility/unique_id_generator/unique_id_generator.hpp"

#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char *argv[]) {

    ResourcePath rp(false);

    Colors colors;

    // FPSCamera fps_camera(glm::vec3(0), .10);
    FPSCamera fps_camera;

    unsigned int window_width_px = 700, window_height_px = 700;
    bool start_in_fullscreen = true;
    bool start_with_mouse_captured = true;
    bool vsync = false;

    Window window;
    window.initialize_glfw_glad_and_return_window(window_width_px, window_height_px, "catmullrom camera interpolation",
                                                  start_in_fullscreen, start_with_mouse_captured, vsync);

    InputState input_state;

    std::vector<ShaderType> requested_shaders = {
        ShaderType::TEXTURE_PACKER_RIGGED_AND_ANIMATED_CWL_V_TRANSFORMATION_UBOS_1024_WITH_TEXTURES};
    ShaderCache shader_cache(requested_shaders);
    Batcher batcher(shader_cache);

    const auto textures_directory = std::filesystem::path("assets");
    std::filesystem::path output_dir = std::filesystem::path("assets") / "packed_textures";
    int container_side_length = 1024;

    TexturePacker texture_packer(textures_directory, output_dir, container_side_length);
    shader_cache.set_uniform(
        ShaderType::TEXTURE_PACKER_RIGGED_AND_ANIMATED_CWL_V_TRANSFORMATION_UBOS_1024_WITH_TEXTURES,
        ShaderUniformVariable::PACKED_TEXTURE_BOUNDING_BOXES, 1);

    std::function<void(unsigned int)> char_callback = [](unsigned int codepoint) {};
    std::function<void(int, int, int, int)> key_callback = [&](int key, int scancode, int action, int mods) {
        input_state.glfw_key_callback(key, scancode, action, mods);
    };
    std::function<void(double, double)> mouse_pos_callback = [&](double xpos, double ypos) {
        fps_camera.mouse_callback(xpos, ypos);
    };
    std::function<void(int, int, int)> mouse_button_callback = [&](int button, int action, int mods) {
        input_state.glfw_mouse_button_callback(button, action, mods);
    };
    std::function<void(int, int)> frame_buffer_size_callback = [](int width, int height) {};
    GLFWLambdaCallbackManager glcm(window.glfw_window, char_callback, key_callback, mouse_pos_callback,
                                   mouse_button_callback, frame_buffer_size_callback);

    glm::mat4 identity = glm::mat4(1);

    std::string path = (argc > 1) ? argv[1] : "assets/animations/sniper_rifle_with_hands.fbx";

    rigged_model_loading::RecIvpntRiggedCollector rirc;
    auto ivpntrs = rirc.parse_model_into_ivpntrs(rp.gfp(path).string());
    auto ivpntprs = texture_packer_model_loading::convert_ivpntr_to_ivpntpr(ivpntrs, texture_packer);

    double current_animation_time = 0;
    bool animation_is_playing = false;

    std::function<void(double)> tick = [&](double dt) {
        /*glfwGetFramebufferSize(window, &width, &height);*/

        glViewport(0, 0, window_width_px, window_height_px);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        fps_camera.process_input(input_state.is_pressed(EKey::LEFT_CONTROL), input_state.is_pressed(EKey::TAB),
                                 input_state.is_pressed(EKey::w), input_state.is_pressed(EKey::a),
                                 input_state.is_pressed(EKey::s), input_state.is_pressed(EKey::d),
                                 input_state.is_pressed(EKey::SPACE), input_state.is_pressed(EKey::LEFT_SHIFT), dt);

        if (input_state.is_just_pressed(EKey::p)) {
            animation_is_playing = not animation_is_playing;
        }

        shader_cache.set_uniform(
            ShaderType::TEXTURE_PACKER_RIGGED_AND_ANIMATED_CWL_V_TRANSFORMATION_UBOS_1024_WITH_TEXTURES,
            ShaderUniformVariable::CAMERA_TO_CLIP, fps_camera.get_projection_matrix(window_width_px, window_height_px));

        shader_cache.set_uniform(
            ShaderType::TEXTURE_PACKER_RIGGED_AND_ANIMATED_CWL_V_TRANSFORMATION_UBOS_1024_WITH_TEXTURES,
            ShaderUniformVariable::WORLD_TO_CAMERA, fps_camera.get_view_matrix());

        // animation start

        // first we upload the animation matrix
        std::vector<glm::mat4> bone_transformations;
        rirc.set_bone_transforms(current_animation_time, bone_transformations, "fire");

        const unsigned int MAX_BONES_TO_BE_USED = 100;
        ShaderProgramInfo shader_info = shader_cache.get_shader_program(
            ShaderType::TEXTURE_PACKER_RIGGED_AND_ANIMATED_CWL_V_TRANSFORMATION_UBOS_1024_WITH_TEXTURES);

        GLint location = glGetUniformLocation(
            shader_info.id, shader_cache.get_uniform_name(ShaderUniformVariable::BONE_ANIMATION_TRANSFORMS).c_str());

        shader_cache.use_shader_program(
            ShaderType::TEXTURE_PACKER_RIGGED_AND_ANIMATED_CWL_V_TRANSFORMATION_UBOS_1024_WITH_TEXTURES);
        glUniformMatrix4fv(location, bone_transformations.size(), GL_FALSE, glm::value_ptr(bone_transformations[0]));

        // now the model geometry:
        for (auto &ivpntpr : ivpntprs) {
            // Populate bone_indices and bone_weights
            std::vector<glm::ivec4> bone_indices;
            std::vector<glm::vec4> bone_weights;

            for (const auto &vertex_bone_data : ivpntpr.bone_data) {
                glm::ivec4 indices(static_cast<int>(vertex_bone_data.indices_of_bones_that_affect_this_vertex[0]),
                                   static_cast<int>(vertex_bone_data.indices_of_bones_that_affect_this_vertex[1]),
                                   static_cast<int>(vertex_bone_data.indices_of_bones_that_affect_this_vertex[2]),
                                   static_cast<int>(vertex_bone_data.indices_of_bones_that_affect_this_vertex[3]));

                glm::vec4 weights(vertex_bone_data.weight_value_of_this_vertex_wrt_bone[0],
                                  vertex_bone_data.weight_value_of_this_vertex_wrt_bone[1],
                                  vertex_bone_data.weight_value_of_this_vertex_wrt_bone[2],
                                  vertex_bone_data.weight_value_of_this_vertex_wrt_bone[3]);

                bone_indices.push_back(indices);
                bone_weights.push_back(weights);
            }

            std::vector<int> packed_texture_indices(ivpntpr.xyz_positions.size(), ivpntpr.packed_texture_index);
            int ptbbi = texture_packer.get_packed_texture_bounding_box_index_of_texture(ivpntpr.texture);
            std::vector<int> packed_texture_bounding_box_indices(ivpntpr.xyz_positions.size(), ptbbi);

            // bad!
            std::vector<unsigned int> ltw_indices(ivpntpr.xyz_positions.size(), ivpntpr.id);

            batcher.texture_packer_rigged_and_animated_cwl_v_transformation_ubos_1024_with_textures_shader_batcher
                .queue_draw(ivpntpr.id, ivpntpr.indices, ltw_indices, bone_indices, bone_weights,
                            packed_texture_indices, ivpntpr.packed_texture_coordinates,
                            packed_texture_bounding_box_indices, ivpntpr.xyz_positions);
        }

        batcher.texture_packer_rigged_and_animated_cwl_v_transformation_ubos_1024_with_textures_shader_batcher
            .upload_ltw_matrices();

        batcher.texture_packer_rigged_and_animated_cwl_v_transformation_ubos_1024_with_textures_shader_batcher
            .draw_everything();

        // animation end

        glfwSwapBuffers(window.glfw_window);
        glfwPollEvents();

        if (animation_is_playing) {
            current_animation_time += dt;
        }

        TemporalBinarySignal::process_all();
    };

    std::function<bool()> termination = [&]() { return glfwWindowShouldClose(window.glfw_window); };

    FixedFrequencyLoop ffl;
    ffl.start(240, tick, termination);

    return 0;
}
