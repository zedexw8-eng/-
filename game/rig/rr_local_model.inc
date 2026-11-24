#include "rr_tung_tung_data.inc"

/* Runs on Unity's camera thread. Only the confirmed local view is edited. */
typedef uint64_t (*LM_NEW_OBJECT)(uint64_t);
typedef uint64_t (*LM_NEW_ARRAY)(uint64_t, size_t);
typedef uint64_t (*LM_STRING)(const char*);
typedef uintptr_t (*LM_HOLD)(uint64_t, unsigned char);
typedef uint64_t (*LM_TARGET)(uintptr_t);
typedef void (*LM_FREE)(uintptr_t);
typedef void (*LM_OBJECT_CALL)(uint64_t, void*);
typedef void (*LM_BOOL)(uint64_t, unsigned char, void*);

static LM_NEW_OBJECT lm_new_object;
static LM_NEW_ARRAY lm_new_array;
static LM_STRING lm_string;
static LM_HOLD lm_hold;
static LM_TARGET lm_target;
static LM_FREE lm_free;

static uintptr_t lm_object, lm_mesh, lm_texture, lm_material;
static uint64_t lm_view;
static unsigned lm_generation;
static uintptr_t lm_hidden[128];
static int lm_hidden_count;
static const char* lm_stage;

static uint64_t lm_class(const char* assembly_name, const char* namespc, const char* name) {
    HMODULE assembly = (HMODULE)(uintptr_t)g_module;
    typedef const void* (*OPEN_ASSEMBLY)(void*, const char*);
    const RR_DOMAIN_GET domain_get = (RR_DOMAIN_GET)GetProcAddress(assembly, "il2cpp_domain_get");
    const OPEN_ASSEMBLY open_assembly = (OPEN_ASSEMBLY)GetProcAddress(assembly, "il2cpp_domain_assembly_open");
    const RR_ASM_IMAGE image_of = (RR_ASM_IMAGE)GetProcAddress(assembly, "il2cpp_assembly_get_image");
    const RR_CLASS_FROM_NAME lookup = (RR_CLASS_FROM_NAME)GetProcAddress(assembly, "il2cpp_class_from_name");
    if (domain_get && open_assembly && image_of && lookup) {
        void* domain = domain_get();
        const void* managed_assembly = domain ? open_assembly(domain, assembly_name) : NULL;
        const void* image = managed_assembly ? image_of(managed_assembly) : NULL;
        const uint64_t klass = image ? (uint64_t)(uintptr_t)lookup(image, namespc, name) : 0;
        if (klass) return klass;
    }
    return resolve_class_api(name);
}

static uint64_t lm_type(const char* name) {
    HMODULE assembly = (HMODULE)(uintptr_t)g_module;
    typedef uint64_t (*TYPE_FN)(uint64_t);
    const TYPE_FN class_type = (TYPE_FN)GetProcAddress(assembly, "il2cpp_class_get_type");
    const TYPE_FN type_object = (TYPE_FN)GetProcAddress(assembly, "il2cpp_type_get_object");
    const uint64_t klass = lm_class("UnityEngine.CoreModule", "UnityEngine", name);
    return klass && class_type && type_object ? type_object(class_type(klass)) : 0;
}

static int lm_body_renderers(uint64_t view, uint64_t* data) {
    static uint64_t type;
    if (!type) type = lm_type("SkinnedMeshRenderer");
    const uint64_t root = UNITY_FN(0x627DFB0u, SC_GET_REF)(view, NULL);
    if (!type || !scene_alive(root)) return 0;
    typedef uint64_t (*GET_CHILDREN)(uint64_t, uint64_t, unsigned char, void*);
    const uint64_t children = UNITY_FN(0x6287E20u, GET_CHILDREN)(root, type, 1, NULL);
    const int count = u_array(children, data);
    return count <= 512 ? count : 0;
}

static uint64_t lm_body_shader(uint64_t view, uint64_t texture_property) {
    uint64_t data = 0;
    uint64_t fallback = 0;
    const int count = lm_body_renderers(view, &data);
    typedef unsigned char (*HAS_PROPERTY)(uint64_t, uint64_t, void*);
    for (int i = 0; i < count; ++i) {
        const uint64_t renderer = rr_u64(data + (uint64_t)i * 8);
        if (!scene_alive(renderer)) continue;
        const uint64_t material = UNITY_FN(0x62124F0u, SC_GET_REF)(renderer, NULL);
        if (!scene_alive(material)) continue;
        const uint64_t shader = UNITY_FN(0x6233B10u, SC_GET_REF)(material, NULL);
        if (!scene_alive(shader)) continue;
        if (UNITY_FN(0x622E820u, HAS_PROPERTY)(material, texture_property, NULL)) return shader;
        if (!fallback) fallback = shader;
    }
    return fallback;
}

static void lm_destroy(uintptr_t* handle) {
    if (!*handle) return;
    const uint64_t object = lm_target(*handle);
    if (scene_alive(object)) UNITY_FN(0x628DFE0u, LM_OBJECT_CALL)(object, NULL);
    lm_free(*handle);
    *handle = 0;
}

static void lm_reset(void) {
    for (int i = 0; i < lm_hidden_count; ++i) {
        const uint64_t renderer = lm_target(lm_hidden[i]);
        if (scene_alive(renderer)) UNITY_FN(0x6214220u, LM_BOOL)(renderer, 0, NULL);
        lm_free(lm_hidden[i]);
    }
    lm_hidden_count = 0;
    lm_destroy(&lm_object);
    lm_destroy(&lm_mesh);
    lm_destroy(&lm_material);
    lm_destroy(&lm_texture);
    lm_view = 0;
}

static uint64_t lm_array(const char* element, const unsigned char* bytes, size_t size, size_t stride) {
    const int system = !strcmp(element, "Int32") || !strcmp(element, "Byte");
    lm_stage = element;
    const uint64_t klass = system ? lm_class("mscorlib", "System", element)
                                  : lm_class("UnityEngine.CoreModule", "UnityEngine", element);
    if (!klass) return 0;
    const uint64_t array = lm_new_array(klass, size / stride);
    if (array) memcpy((void*)(uintptr_t)(array + 0x20), bytes, size);
    return array;
}

static int lm_mesh_data(uint64_t mesh) {
    const uint64_t vertices = lm_array("Vector3", model_positions, sizeof model_positions, 12);
    if (!vertices) return 0;
    UNITY_FN(0x6249DC0u, SC_SET_REF)(mesh, vertices, NULL);
    const uint64_t normals = lm_array("Vector3", model_normals, sizeof model_normals, 12);
    if (!normals) return 0;
    UNITY_FN(0x6249700u, SC_SET_REF)(mesh, normals, NULL);
    const uint64_t uv = lm_array("Vector2", model_uv, sizeof model_uv, 8);
    if (!uv) return 0;
    UNITY_FN(0x6249C90u, SC_SET_REF)(mesh, uv, NULL);
    const uint64_t triangles = lm_array("Int32", model_triangles, sizeof model_triangles, 4);
    if (!triangles) return 0;
    UNITY_FN(0x6249890u, SC_SET_REF)(mesh, triangles, NULL);
    UNITY_FN(0x6240330u, LM_OBJECT_CALL)(mesh, NULL);
    return 1;
}

static int lm_create(uint64_t view) {
    lm_stage = "IL2CPP exports";
    HMODULE assembly = (HMODULE)(uintptr_t)g_module;
    lm_new_object = (LM_NEW_OBJECT)GetProcAddress(assembly, "il2cpp_object_new");
    lm_new_array = (LM_NEW_ARRAY)GetProcAddress(assembly, "il2cpp_array_new");
    lm_string = (LM_STRING)GetProcAddress(assembly, "il2cpp_string_new");
    lm_hold = (LM_HOLD)GetProcAddress(assembly, "il2cpp_gchandle_new");
    lm_target = (LM_TARGET)GetProcAddress(assembly, "il2cpp_gchandle_get_target");
    lm_free = (LM_FREE)GetProcAddress(assembly, "il2cpp_gchandle_free");
    if (!lm_new_object || !lm_new_array || !lm_string || !lm_hold || !lm_target || !lm_free) return 0;

    lm_stage = "Unity classes and local transform";
    const uint64_t mesh_class = lm_class("UnityEngine.CoreModule", "UnityEngine", "Mesh");
    const uint64_t texture_class = lm_class("UnityEngine.CoreModule", "UnityEngine", "Texture2D");
    const uint64_t material_class = lm_class("UnityEngine.CoreModule", "UnityEngine", "Material");
    const uint64_t object_class = lm_class("UnityEngine.CoreModule", "UnityEngine", "GameObject");
    const uint64_t filter_type = lm_type("MeshFilter");
    const uint64_t renderer_type = lm_type("MeshRenderer");
    const uint64_t parent = UNITY_FN(0x627E130u, SC_GET_REF)(view, NULL);
    if (!mesh_class || !material_class || !object_class ||
        !filter_type || !renderer_type || !scene_alive(parent)) return 0;

    lm_stage = "mesh construction";
    const uint64_t mesh = lm_new_object(mesh_class);
    if (!mesh || !(lm_mesh = lm_hold(mesh, 0))) return 0;
    UNITY_FN(0x6248080u, LM_OBJECT_CALL)(mesh, NULL);
    if (!scene_alive(mesh) || !lm_mesh_data(mesh)) return 0;

    lm_stage = "texture construction";
    const uint64_t texture = texture_class ? lm_new_object(texture_class) : 0;
    int textured = 0;
    if (texture && (lm_texture = lm_hold(texture, 0))) {
        typedef void (*TEXTURE_CTOR)(uint64_t, int, int, void*);
        UNITY_FN(0x6262940u, TEXTURE_CTOR)(texture, 2, 2, NULL);
        const uint64_t png = lm_array("Byte", model_png, sizeof model_png, 1);
        typedef unsigned char (*LOAD_IMAGE)(uint64_t, uint64_t, void*);
        textured = png && scene_alive(texture) && UNITY_FN(0x62FC9C0u, LOAD_IMAGE)(texture, png, NULL);
    }
    if (!textured) {
        lm_destroy(&lm_texture);
        rr_int_log("model > Tung Tung texture unavailable, using plain material");
    }

    lm_stage = "shader lookup";
    uint64_t shader = UNITY_FN(0x62171E0u, SC_GET_REF)(lm_string("Universal Render Pipeline/Unlit"), NULL);
    if (!scene_alive(shader))
        shader = UNITY_FN(0x62171E0u, SC_GET_REF)(lm_string("Universal Render Pipeline/Lit"), NULL);
    if (!scene_alive(shader))
        shader = lm_body_shader(view, lm_string("_BaseMap"));
    if (!scene_alive(shader)) return 0;
    lm_stage = "material construction";
    const uint64_t material = lm_new_object(material_class);
    if (!material || !(lm_material = lm_hold(material, 0))) return 0;
    UNITY_FN(0x6232E40u, SC_SET_REF)(material, shader, NULL);
    if (!scene_alive(material)) return 0;
    typedef void (*SET_TEXTURE)(uint64_t, uint64_t, uint64_t, void*);
    if (textured) {
        typedef unsigned char (*HAS_PROPERTY)(uint64_t, uint64_t, void*);
        const char* properties[] = {"_BaseMap", "_MainTex"};
        for (int i = 0; i < _countof(properties); ++i) {
            const uint64_t property = lm_string(properties[i]);
            if (UNITY_FN(0x622E820u, HAS_PROPERTY)(material, property, NULL))
                UNITY_FN(0x6232560u, SET_TEXTURE)(material, property, texture, NULL);
        }
    }

    lm_stage = "GameObject construction";
    const uint64_t object = lm_new_object(object_class);
    if (!object || !(lm_object = lm_hold(object, 0))) return 0;
    UNITY_FN(0x6289CF0u, SC_SET_REF)(object, lm_string("Tung Tung Tung Sahur"), NULL);
    if (!scene_alive(object)) return 0;
    lm_stage = "MeshFilter and MeshRenderer";
    typedef uint64_t (*ADD_COMPONENT)(uint64_t, uint64_t, void*);
    const uint64_t filter = UNITY_FN(0x62860D0u, ADD_COMPONENT)(object, filter_type, NULL);
    const uint64_t renderer = UNITY_FN(0x62860D0u, ADD_COMPONENT)(object, renderer_type, NULL);
    const uint64_t transform = UNITY_FN(0x628A7A0u, SC_GET_REF)(object, NULL);
    if (!scene_alive(filter) || !scene_alive(renderer) || !scene_alive(transform)) return 0;
    UNITY_FN(0x6235F80u, SC_SET_REF)(filter, mesh, NULL);
    UNITY_FN(0x6212D50u, SC_SET_REF)(renderer, material, NULL);
    typedef void (*SET_PARENT)(uint64_t, uint64_t, unsigned char, void*);
    UNITY_FN(0x62CE560u, SET_PARENT)(transform, parent, 0, NULL);
    return 1;
}

static void local_model_frame(int enabled) {
    static DWORD last_check;
    static uint64_t failed_view;
    const DWORD now = GetTickCount();
    if (now - last_check < 250) return;
    last_check = now;
    const unsigned generation = world_gen();
    const uint64_t local = enabled && PL(g_world) ? rr_local_player() : 0;
    const uint64_t view = local ? local_character_view(local) : 0;
    if (generation != lm_generation || view != lm_view || !enabled) lm_reset();
    if (generation != lm_generation || !enabled) failed_view = 0;
    lm_generation = generation;
    if (!scene_alive(view) || view == failed_view) return;
    if (!lm_object) {
        if (!lm_create(view)) {
            lm_reset();
            failed_view = view;
            rr_int_log("model > Tung Tung setup failed at %s", lm_stage);
            return;
        }
        lm_view = view;
        rr_int_log("model > Tung Tung attached to local player");
    }

    uint64_t data = 0;
    const int count = lm_body_renderers(view, &data);
    for (int i = 0; i < count && lm_hidden_count < _countof(lm_hidden); ++i) {
        const uint64_t renderer = rr_u64(data + (uint64_t)i * 8);
        typedef unsigned char (*GET_BOOL)(uint64_t, void*);
        if (!scene_alive(renderer) || UNITY_FN(0x62134A0u, GET_BOOL)(renderer, NULL)) continue;
        int tracked = 0;
        for (int j = 0; j < lm_hidden_count; ++j) {
            if (lm_target(lm_hidden[j]) == renderer) { tracked = 1; break; }
        }
        if (!tracked) {
            const uintptr_t handle = lm_hold(renderer, 0);
            if (!handle) continue;
            lm_hidden[lm_hidden_count++] = handle;
        }
        UNITY_FN(0x6214220u, LM_BOOL)(renderer, 1, NULL);
    }
}
