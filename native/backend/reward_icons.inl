#pragma once

#include <cmath>

namespace vst::detail {

template <class Host, class Api>
void RewardBackend<Host, Api>::ResolveIcons(const void* game_image) {
    const void* unity = A().FindImage("UnityEngine.CoreModule.dll");
    if (!unity || !A().object_new || !A().array_length || !A().array_get_byte_length
        || !A().array_object_header_size) return;
    auto u = [&](const char* name) { return C(unity, "UnityEngine", name); };
    icon.texture2d = u("Texture2D");
    icon.color32 = u("Color32");
    icon.key = F(header_class, "<texture>k__BackingField", "System.String");
    icon.sprite = M(C(game_image, "VampireSurvivors.Graphics", "SpriteManager"), "GetSprite", "UnityEngine.Sprite",
        {"System.String", "System.String", "System.Boolean"}, 100666971, true);
    icon.texture = M(u("Sprite"), "get_texture", "UnityEngine.Texture2D", {}, 100663776);
    icon.rect = M(u("Sprite"), "get_textureRect", "UnityEngine.Rect", {}, 100663788);
    icon.rotation = M(u("Sprite"), "get_packingRotation", "UnityEngine.SpritePackingRotation", {}, 100663787);
    icon.width = M(u("Texture"), "get_width", "System.Int32", {}, 100666668);
    icon.height = M(u("Texture"), "get_height", "System.Int32", {}, 100666670);
    auto render_texture = u("RenderTexture");
    icon.create = M(render_texture, "GetTemporary", "UnityEngine.RenderTexture",
        {"System.Int32", "System.Int32", "System.Int32", "UnityEngine.RenderTextureFormat", "UnityEngine.RenderTextureReadWrite"}, 100667187, true);
    icon.release = M(render_texture, "ReleaseTemporary", "System.Void", {"UnityEngine.RenderTexture"}, 100667146, true);
    icon.active = M(render_texture, "get_active", "UnityEngine.RenderTexture", {}, 100667118, true);
    icon.set_active = M(render_texture, "set_active", "System.Void", {"UnityEngine.RenderTexture"}, 100667119, true);
    icon.blit = M(u("Graphics"), "Blit", "System.Void",
        {"UnityEngine.Texture", "UnityEngine.RenderTexture", "UnityEngine.Vector2", "UnityEngine.Vector2"}, 100664861, true);
    icon.construct = M(icon.texture2d, ".ctor", "System.Void",
        {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"}, 100666812);
    icon.read = M(icon.texture2d, "ReadPixels", "System.Void",
        {"UnityEngine.Rect", "System.Int32", "System.Int32", "System.Boolean"}, 100666841);
    icon.pixels = M(icon.texture2d, "GetPixels32", "UnityEngine.Color32[]", {"System.Int32"}, 100666786);
    icon.destroy = M(u("Object"), "Destroy", "System.Void", {"UnityEngine.Object"}, 100668573, true);
    icons_ready = icon.texture2d && icon.color32 && icon.key && icon.sprite && icon.texture && icon.rect
        && icon.rotation && icon.width && icon.height && icon.create && icon.release && icon.active && icon.set_active
        && icon.blit && icon.construct && icon.read && icon.pixels && icon.destroy;
}

template <class Host, class Api>
typename RewardBackend<Host, Api>::IconPixels RewardBackend<Host, Api>::ReadIconPixels(const CatalogItem& item) {
    Root source(A(), Object(converted, catalog_owner.Get()));
    Root levels(A(), DictObject(source.Get(), kConverted, kDataList, item.type));
    const int32_t count = ListCount(levels.Get(), kDataList, 1000);
    Root header;
    // Catalog creation uses the first non-null level as the item header.
    for (int32_t i = 0; i < std::min(count, 16); ++i) {
        header = Root(A(), ListObject(levels.Get(), kDataList, kWeaponData, i));
        if (A().IsA(header.Get(), header_class)) break;
    }
    if (!A().IsA(header.Get(), header_class)) return {};
    Root frame(A(), Field<void*>(header.Get(), header_frame));
    Root key(A(), Field<void*>(header.Get(), icon.key));
    if (!frame.Get() || !key.Get()) return {};
    bool ignore_extension = true;
    void* sprite_args[]{frame.Get(), key.Get(), &ignore_extension};
    Root sprite(A(), Object(icon.sprite, nullptr, sprite_args));
    if (!sprite.Get()) return {};
    Root texture(A(), Object(icon.texture, sprite.Get()));
    if (!A().IsA(texture.Get(), icon.texture2d)) return {};
    auto texture_dimension = [&](const void* method) {
        void* box = nullptr;
        if (!A().Invoke(method, texture.Get(), nullptr, box, true) || !box) return int32_t{0};
        void* data = A().object_unbox(box);
        int32_t result = 0;
        if (IsReadableMemory(data, sizeof(result))) std::memcpy(&result, data, sizeof(result));
        return result;
    };
    const int32_t source_width = texture_dimension(icon.width), source_height = texture_dimension(icon.height);
    struct Rect { float x, y, w, h; };
    struct Vec2 { float x, y; };
    const Rect rect = Scalar<Rect>(icon.rect, sprite.Get());
    if (source_width <= 0 || source_height <= 0 || !std::isfinite(rect.x) || !std::isfinite(rect.y)
        || !std::isfinite(rect.w) || !std::isfinite(rect.h) || rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0
        || rect.x + rect.w > source_width || rect.y + rect.h > source_height) return {};
    Vec2 scale{rect.w / source_width, rect.h / source_height}, offset{rect.x / source_width, rect.y / source_height};
    int32_t rotation = Scalar<int32_t>(icon.rotation, sprite.Get());
    if (rotation < 0 || rotation > 3) return {};
    if (rotation & 1) { offset.x += scale.x; scale.x = -scale.x; }
    if (rotation & 2) { offset.y += scale.y; scale.y = -scale.y; }
    float factor = std::min(1.0f, 32.0f / std::max(rect.w, rect.h));
    int32_t width = std::clamp(static_cast<int32_t>(std::lround(rect.w * factor)), 1, 32);
    int32_t height = std::clamp(static_cast<int32_t>(std::lround(rect.h * factor)), 1, 32);
    int32_t zero = 0, argb32 = 0, srgb = 2, rgba32 = 4;
    bool mipmaps = false;
    Root previous(A(), Object(icon.active, nullptr));
    void* temporary_args[]{&width, &height, &zero, &argb32, &srgb};
    void* temporary = Object(icon.create, nullptr, temporary_args);
    if (!temporary) return {};
    Root target, readback;
    void* readback_raw = nullptr;
    Finally restore([&] {
        void* unused = nullptr;
        void* old_args[]{previous.Get()};
        A().Invoke(icon.set_active, nullptr, old_args, unused);
        void* release_args[]{target.Get() ? target.Get() : temporary};
        A().Invoke(icon.release, nullptr, release_args, unused);
        if (readback_raw) {
            void* destroy_args[]{readback.Get() ? readback.Get() : readback_raw};
            A().Invoke(icon.destroy, nullptr, destroy_args, unused);
        }
    });
    target = Root(A(), temporary);
    readback_raw = A().object_new(icon.texture2d);
    if (!readback_raw) return {};
    readback = Root(A(), readback_raw);
    void* construct_args[]{&width, &height, &rgba32, &mipmaps};
    Object(icon.construct, readback.Get(), construct_args);
    void* blit_args[]{texture.Get(), target.Get(), &scale, &offset};
    Object(icon.blit, nullptr, blit_args);
    void* active_args[]{target.Get()};
    Object(icon.set_active, nullptr, active_args);
    Rect target_rect{0, 0, static_cast<float>(width), static_cast<float>(height)};
    void* read_args[]{&target_rect, &zero, &zero, &mipmaps};
    Object(icon.read, readback.Get(), read_args);
    void* pixels_args[]{&zero};
    Root pixels(A(), Object(icon.pixels, readback.Get(), pixels_args));
    const uint32_t bytes = static_cast<uint32_t>(width * height * 4);
    const uint32_t header_size = A().array_object_header_size();
    if (!pixels.Get() || A().class_get_element_class(A().object_get_class(pixels.Get())) != icon.color32
        || A().array_length(pixels.Get()) != bytes / 4 || A().array_get_byte_length(pixels.Get()) != bytes
        || header_size < sizeof(void*) * 2 || header_size > 128) return {};
    auto rgba = static_cast<const unsigned char*>(pixels.Get()) + header_size;
    if (!IsReadableMemory(rgba, bytes)) return {};
    IconPixels result{{}, width, height};
    result.rgba_hex.reserve(bytes * 2);
    constexpr char hex[] = "0123456789abcdef";
    // Unity returns bottom-up rows. The client consumes top-down RGBA.
    for (int32_t y = height - 1; y >= 0; --y) {
        for (int32_t x = 0; x < width * 4; ++x) {
            unsigned char channel = rgba[y * width * 4 + x];
            result.rgba_hex.push_back(hex[channel >> 4]);
            result.rgba_hex.push_back(hex[channel & 15]);
        }
    }
    return result;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::ReadIcon(const std::string& id, Response& result) {
    result.has_item_icon = true;
    result.item_icon_id = id;
    if (!icons_ready || !Named(id)) return;
    if (!RefreshCatalog()) { result.item_icon_pending = true; return; }
    auto found = by_id.find(id);
    if (found == by_id.end()) return;
    auto cached = icons.find(id);
    if (cached == icons.end()) {
        uint64_t now = GetTickCount64();
        if (now < next_icon_read) { result.item_icon_pending = true; return; }
        next_icon_read = now + 160;
        if (icons.size() >= 512) icons.clear();
        IconPixels pixels;
        try { pixels = ReadIconPixels(catalog[found->second]); }
        catch (const std::exception&) {} // Missing art leaves a normal text row.
        cached = icons.emplace(id, std::move(pixels)).first;
    }
    result.item_icon_width = cached->second.width;
    result.item_icon_height = cached->second.height;
    result.item_icon_rgba_hex = cached->second.rgba_hex;
}

} // namespace vst::detail
