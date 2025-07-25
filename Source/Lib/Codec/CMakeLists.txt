#
# Copyright(c) 2019 Intel Corporation
#
# This source code is subject to the terms of the BSD 2 Clause License and
# the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
# was not distributed with this source code in the LICENSE file, you can
# obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
# Media Patent License 1.0 was not distributed with this source code in the
# PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
#

# Codec Directory CMakeLists.txt

if(EXCLUDE_HASH)
    set(PACKAGE_VERSION_STRING ${PROJECT_VERSION}${VERSION_TAG})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/EbVersion.h.in EbVersion.h @ONLY)
    add_custom_target(EbVersionHeaderGen)
else()
    add_custom_target(EbVersionHeaderGen
        ${CMAKE_COMMAND}
            -D INPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/EbVersion.h.in
            -D OUTPUT_FILE=EbVersion.h
            -D GIT_ROOT_DIR=${PROJECT_SOURCE_DIR}
            -D PACKAGE_VERSION_STRING=${PROJECT_VERSION}${VERSION_TAG}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/ConfigureGitVersion.cmake
        BYPRODUCTS EbVersion.h
        COMMENT "Generating version header"
        VERBATIM)
endif()

if(NOT COMPILE_C_ONLY AND HAVE_X86_PLATFORM)
# Include Encoder Subdirectories
include_directories(${PROJECT_SOURCE_DIR}/Source/API/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Globals/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Codec/
    ${PROJECT_SOURCE_DIR}/Source/Lib/C_DEFAULT/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_SSE2/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_SSSE3/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_SSE4_1/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_AVX2/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_AVX512/
    ${PROJECT_SOURCE_DIR}/third_party/fastfeat/)
elseif(NOT COMPILE_C_ONLY AND HAVE_ARM_PLATFORM)
include_directories(${PROJECT_SOURCE_DIR}/Source/API/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Globals/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Codec/
    ${PROJECT_SOURCE_DIR}/Source/Lib/C_DEFAULT/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_NEON/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_NEON_DOTPROD/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_NEON_I8MM/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_SVE/
    ${PROJECT_SOURCE_DIR}/Source/Lib/ASM_SVE2/
    ${PROJECT_SOURCE_DIR}/third_party/fastfeat/)
else ()
# Include Encoder Subdirectories
include_directories(${PROJECT_SOURCE_DIR}/Source/API/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Globals/
    ${PROJECT_SOURCE_DIR}/Source/Lib/Codec/
    ${PROJECT_SOURCE_DIR}/Source/Lib/C_DEFAULT/
    ${PROJECT_SOURCE_DIR}/third_party/fastfeat/)
endif ()

set(all_files
        adaptive_mv_pred.c
        adaptive_mv_pred.h
        aom_dsp_rtcd.c
        aom_dsp_rtcd.h
        av1_common.h
        av1_structs.h
        av1me.c
        av1me.h
        bitstream_unit.c
        bitstream_unit.h
        blend_a64_mask.c
        block_structures.c
        block_structures.h
        cabac_context_model.c
        cabac_context_model.h
        cdef.c
        cdef.h
        cdef_process.c
        cdef_process.h
        coding_loop.c
        coding_loop.h
        coding_unit.c
        coding_unit.h
        coefficients.h
        common_dsp_rtcd.c
        common_dsp_rtcd.h
        common_utils.h
        compute_mean.h
        compute_sad.h
        convolve.c
        convolve.h
        corner_detect.c
        corner_detect.h
        corner_match.c
        corner_match.h
        deblocking_common.c
        deblocking_common.h
        deblocking_filter.c
        deblocking_filter.h
        definitions.h
        dlf_process.c
        dlf_process.h
        dwt.c
        dwt.h
        enc_cdef.c
        enc_cdef.h
        enc_dec_process.c
        enc_dec_process.h
        enc_dec_results.c
        enc_dec_results.h
        enc_dec_segments.c
        enc_dec_segments.h
        enc_dec_tasks.c
        enc_dec_tasks.h
        enc_inter_prediction.c
        enc_inter_prediction.h
        enc_intra_prediction.c
        enc_intra_prediction.h
        enc_mode_config.c
        enc_mode_config.h
        enc_warped_motion.c
        enc_warped_motion.h
        encode_context.c
        encode_context.h
        encoder.h
        entropy_coding.c
        entropy_coding.h
        ec_object.h
        ec_process.c
        ec_process.h
        ec_results.c
        ec_results.h
        fft.c
        fft_common.h
        filter.h
        firstpass.c
        firstpass.h
        full_loop.c
        full_loop.h
        global_motion.c
        global_motion.h
        global_me.c
        global_me.h
        global_me_cost.c
        global_me_cost.h
        grainSynthesis.c
        grainSynthesis.h
        hash.c
        hash.h
        hash_motion.c
        hash_motion.h
        initial_rc_process.c
        initial_rc_process.h
        initial_rc_reorder_queue.c
        initial_rc_reorder_queue.h
        initial_rc_results.c
        initial_rc_results.h
        inter_prediction.c
        inter_prediction.h
        intra_prediction.c
        intra_prediction.h
        inv_transforms.c
        inv_transforms.h
        k_means_template.h
        lambda_rate_tables.h
        mathutils.h
        mcomp.c
        mcomp.h
        md_rate_estimation.c
        md_rate_estimation.h
        me_sad_calculation.c
        me_sad_calculation.h
        mode_decision.c
        mode_decision.h
        md_config_process.c
        md_config_process.h
        md_process.c
        md_process.h
        motion_estimation.c
        motion_estimation.h
        me_context.c
        me_context.h
        me_process.c
        me_process.h
        me_results.c
        me_results.h
        me_sb_results.h
        motion_vector_unit.h
        mv.h
        neighbor_arrays.c
        neighbor_arrays.h
        noise_model.c
        noise_model.h
        noise_util.c
        noise_util.h
        object.h
        packetization_process.c
        packetization_process.h
        packetization_reorder_queue.c
        packetization_reorder_queue.h
        palette.c
        pass2_strategy.c
        pass2_strategy.h
        pic_analysis_process.c
        pic_analysis_process.h
        pic_analysis_results.c
        pic_analysis_results.h
        pic_buffer_desc.c
        pic_buffer_desc.h
        pcs.c
        pcs.h
        pd_process.c
        pd_process.h
        pd_queue.c
        pd_queue.h
        pd_reorder_queue.c
        pd_reorder_queue.h
        pd_results.c
        pd_results.h
        pic_demux_results.c
        pic_demux_results.h
        pic_manager_process.c
        pic_manager_process.h
        pic_manager_queue.c
        pic_manager_queue.h
        pic_operators.c
        pic_operators.h
        pred_structure.c
        pred_structure.h
        product_coding_loop.c
        q_matrices.h
        random.h
        ransac.c
        ransac.h
        rc_process.c
        rc_process.h
        rc_results.c
        rc_results.h
        rc_tasks.c
        rc_tasks.h
        rd_cost.c
        rd_cost.h
        reference_object.c
        reference_object.h
        resize.c
        resize.h
        resource_coordination_process.c
        resource_coordination_process.h
        resource_coordination_results.c
        resource_coordination_results.h
        rest_process.c
        rest_process.h
        restoration.c
        restoration.h
        restoration_pick.c
        restoration_pick.h
        segmentation.c
        segmentation.h
        segmentation_params.c
        segmentation_params.h
        sequence_control_set.c
        sequence_control_set.h
        src_ops_process.c
        src_ops_process.h
        super_res.c
        super_res.h
        svt_log.c
        svt_log.h
        svt_malloc.c
        svt_malloc.h
        svt_psnr.c
        svt_psnr.h
        svt_threads.c
        svt_threads.h
        svt_time.c
        svt_time.h
        sys_resource_manager.c
        sys_resource_manager.h
        temporal_filtering.c
        temporal_filtering.h
        transforms.c
        transforms.h
        utility.c
        utility.h
        vector.c
        vector.h
        warped_motion.c
        warped_motion.h
        )

add_library(CODEC OBJECT ${all_files})
