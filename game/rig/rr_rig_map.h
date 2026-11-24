/* rr_rig_map.h - Check joint ancestry before binding a native rig palette. */
#ifndef RR_RIG_MAP_H
#define RR_RIG_MAP_H
#include "rr_cham_model.h"

/* The joints a pose cannot exist without: the anchored spine, the head, and
 * the four limb chains that give a body its extent. Helper joints are not in
 * here - M_head_2_jnt, both arm twists, fingers, face and props draw when the
 * rig ships them. Requiring those dropped the entire palette on meshes that
 * ship without them, which is why a mapped character showed no skeleton. */
static int rr_rig_required_joint(int b) {
    if (b >= 0 && b <= RR_RIG_M_HEAD_1_JNT) return 1;
    if (b >= RR_RIG_L_ARM_0_JNT && b <= RR_RIG_L_ARM_3_JNT) return 1;
    if (b >= RR_RIG_R_ARM_0_JNT && b <= RR_RIG_R_ARM_3_JNT) return 1;
    if (b >= RR_RIG_L_LEG_1_JNT && b <= RR_RIG_L_FOOT_1_JNT) return 1;
    if (b >= RR_RIG_R_LEG_1_JNT && b <= RR_RIG_R_FOOT_1_JNT) return 1;
    return 0;
}

/* The joints an ESP skeleton is built from: torso, head, upper limbs and legs.
 * Facial, finger and attachment joints are optional - a body mesh that ships
 * without any single one of them used to fail the whole palette mapping and
 * dropped the character back to a straight seven-point spine estimate. */
static int rr_rig_skeleton_joint(int b) {
    if (b >= 0 && b <= RR_RIG_M_HEAD_1_JNT) return 1;
    if ((b >= RR_RIG_L_ARM_0_JNT && b <= RR_RIG_L_ARM_3_JNT) || b == RR_RIG_L_ARMTWIST_JNT) return 1;
    if ((b >= RR_RIG_R_ARM_0_JNT && b <= RR_RIG_R_ARM_3_JNT) || b == RR_RIG_R_ARMTWIST_JNT) return 1;
    if (b >= RR_RIG_L_LEG_1_JNT && b <= RR_RIG_L_FOOT_1_JNT) return 1;
    if (b >= RR_RIG_R_LEG_1_JNT && b <= RR_RIG_R_FOOT_1_JNT) return 1;
    return 0;
}

/* Native hierarchies can contain several characters. A joint must belong to
 * this character's anchored spine subtree, including intermediate objects. */
static int rr_rig_descendant(const int* parents, int count, int index, int ancestor) {
    for (int depth=0; depth<128 && index>=0 && index<count; ++depth) {
        if (index==ancestor) return 1;
        index=parents[index];
    }
    return 0;
}

/* Check the nearest mapped ancestor, not just the direct native parent:
 * bag joints have an intervening bag_placeholder that is not a skin bone. */
static int rr_rig_parent_valid(int b,const int* map,const int* parents,int count) {
    if(map[b]<0 || map[b]>=count)return 0;
    int expected=rr_cham_bones[b].parent;
    while(expected>=0 && map[expected]<0)expected=rr_cham_bones[expected].parent;
    if(expected<0)return 1;
    int parent=parents[map[b]];
    for(int depth=0;depth<128 && parent>=0 && parent<count;++depth) {
        for(int other=0;other<RR_CHAM_BONES;++other)
            if(map[other]==parent)return other==expected;
        parent=parents[parent];
    }
    return 0;
}
static void rr_rig_prune_optional(int* map,const int* parents,int count) {
    for(int pass=0;pass<RR_CHAM_BONES;++pass) {
        int changed=0;
        for(int b=0;b<RR_CHAM_BONES;++b)
            if(!rr_rig_required_joint(b) && map[b]>=0 && !rr_rig_parent_valid(b,map,parents,count)) {
                map[b]=-1;changed=1;
            }
        if(!changed)break;
    }
}
static int rr_rig_mapping_valid(const int* map, const int* parents, int count) {
    for (int b=0; b<RR_CHAM_BONES; ++b) {
        int index=map[b];
        if (index<0) { if(rr_rig_required_joint(b))return 0; continue; }
        if (index>=count) return 0;
        for (int other=0; other<b; ++other) if (map[other]==index) return 0;
        if(!rr_rig_parent_valid(b,map,parents,count))return 0;
    }
    return 1;
}
#endif
