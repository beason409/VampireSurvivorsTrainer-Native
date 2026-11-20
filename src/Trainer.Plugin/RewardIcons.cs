using Trainer.Shared;
#if GAME_INTEROP
using UnityEngine;
using VampireSurvivors.Data;
using VampireSurvivors.Graphics;
#endif

namespace Trainer.Plugin;

internal sealed partial class GameAdapter
{
#if GAME_INTEROP
    private const int RewardIconSize = 32;
    private const int RewardIconCacheLimit = 4096;
    private const int RewardIconPngLimit = 32768;
    private IntPtr _rewardIconOwner;
    private int _rewardIconSourceCount;
    private readonly Dictionary<string, byte[]> _rewardItemIcons = new(StringComparer.Ordinal);
#endif

    // Advertise only after the real, verified game's catalog has been read.
    // Older/bootstrap plugins therefore do not cause speculative icon requests.
    internal void EnableRewardIconSupport()
    {
#if GAME_INTEROP
        if (SafeMode && _rewardCatalogOwner != IntPtr.Zero && _rewardCatalog.Count > 0)
            Diagnostics["chest.catalog.icons"] = "supported";
#endif
    }

    /// <summary>Runs on the Unity command queue; an unavailable picture is a normal empty result.</summary>
    internal RewardItemIcon GetRewardItemIcon(string? id)
    {
        var result = new RewardItemIcon();
#if GAME_INTEROP
        if (!IsNamedRewardId(id)) return result;
        result.Id = id!;
        if (!SafeMode) return result;
        try
        {
            _ = GetRewardItemCatalog();
            if (!TryGetRewardType(id!, out var type)) return result;
            if (_rewardIconOwner != _rewardCatalogOwner || _rewardIconSourceCount != _rewardCatalogSourceCount)
            {
                _rewardItemIcons.Clear();
                _rewardIconOwner = _rewardCatalogOwner;
                _rewardIconSourceCount = _rewardCatalogSourceCount;
            }
            if (_rewardItemIcons.TryGetValue(id!, out var cached))
            {
                result.Png = cached;
                return result;
            }

            // GetRewardItemCatalog just validated this owner on this Unity frame.
            // Reuse its wrapper rather than searching the scene for every row.
            var dataManager = new DataManager(_rewardCatalogOwner);
            var source = dataManager.GetConvertedWeapons();
            if (source is null || !source.TryGetValue(type, out var levels) || levels is null) return result;
            for (int index = 0; index < levels.Count; index++)
            {
                var header = levels[index];
                if (header is null || string.IsNullOrWhiteSpace(header.frameName)) continue;
                // Use the same atlas/frame keys as the game. These are shared
                // game assets: neither the sprite nor its texture is modified.
                var sprite = SpriteManager.GetSprite(header.frameName, header.texture);
                if (sprite is null || sprite.Pointer == IntPtr.Zero) return result;
                var png = EncodeRewardIcon(sprite);
                if (png.Length is > 0 and <= RewardIconPngLimit)
                {
                    result.Png = png;
                    if (_rewardItemIcons.Count < RewardIconCacheLimit) _rewardItemIcons[id!] = png;
                }
                return result;
            }
        }
        catch (Exception)
        {
            // Missing/unloaded DLC art, an unsupported packed frame, or a GPU
            // readback failure must never disable an unrelated trainer effect.
        }
#endif
        return result;
    }

#if GAME_INTEROP
    private static byte[] EncodeRewardIcon(Sprite sprite)
    {
        var source = sprite.texture;
        if (source is null || source.Pointer == IntPtr.Zero || source.width <= 0 || source.height <= 0)
            return Array.Empty<byte>();
        // textureRect is the atlas rectangle, unlike sprite.rect's logical size.
        // Unity may reject it for tightly packed non-rectangular geometry; leave
        // those pictures unavailable rather than crop an unrelated atlas region.
        var rect = sprite.textureRect;
        if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0
            || rect.xMax > source.width || rect.yMax > source.height)
            return Array.Empty<byte>();
        float scaleX = rect.width / source.width, scaleY = rect.height / source.height;
        float offsetX = rect.x / source.width, offsetY = rect.y / source.height;
        switch (sprite.packingRotation)
        {
            case SpritePackingRotation.None: break;
            case SpritePackingRotation.FlipHorizontal:
                offsetX += scaleX; scaleX = -scaleX; break;
            case SpritePackingRotation.FlipVertical:
                offsetY += scaleY; scaleY = -scaleY; break;
            case SpritePackingRotation.Rotate180:
                offsetX += scaleX; scaleX = -scaleX;
                offsetY += scaleY; scaleY = -scaleY; break;
            default: return Array.Empty<byte>();
        }
        float factor = Math.Min(1f, RewardIconSize / Math.Max(rect.width, rect.height));
        int width = Math.Clamp((int)Math.Round(rect.width * factor), 1, RewardIconSize);
        int height = Math.Clamp((int)Math.Round(rect.height * factor), 1, RewardIconSize);
        RenderTexture? temporary = null;
        Texture2D? readback = null;
        var previousTarget = RenderTexture.active;
        bool previousSrgbWrite = GL.sRGBWrite;
        try
        {
            temporary = RenderTexture.GetTemporary(width, height, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
            temporary.filterMode = FilterMode.Point;
            GL.sRGBWrite = QualitySettings.activeColorSpace == ColorSpace.Linear;
            // GPU sampling works even when the game's source atlas is not CPU-readable.
            Graphics.Blit(source, temporary, new Vector2(scaleX, scaleY), new Vector2(offsetX, offsetY));
            RenderTexture.active = temporary;
            readback = new Texture2D(width, height, TextureFormat.RGBA32, false);
            readback.ReadPixels(new Rect(0, 0, width, height), 0, 0, false);
            var encoded = ImageConversion.EncodeToPNG(readback);
            return encoded is not null && encoded.Length <= RewardIconPngLimit ? encoded.ToArray() : Array.Empty<byte>();
        }
        finally
        {
            RenderTexture.active = previousTarget;
            GL.sRGBWrite = previousSrgbWrite;
            if (temporary is not null) RenderTexture.ReleaseTemporary(temporary);
            if (readback is not null) UnityEngine.Object.Destroy(readback);
        }
    }
#endif
}
