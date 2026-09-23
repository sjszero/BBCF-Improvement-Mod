#pragma once
#include "imgui.h"

#include <string>
#include <vector>

// Sprite thumbnails for the palette grid.
//
// One idle sprite is embedded per character as palette indices (see
// tools/build_palette_thumbnails.py). Every palette of that character draws the same
// sprite; only the 256 colours differ. So a thumbnail is made by running the palette
// over those indices and uploading the result as a texture.
//
// Textures are built on demand and kept in a small LRU cache, because a player with a
// few hundred palettes would otherwise be asking for a few hundred textures at roughly
// 125 KB each - far past what a 32-bit process should be holding for a preview grid.
// Only what the grid actually draws gets built.
struct IDirect3DDevice9;

namespace PaletteThumbnails
{
	// Hands over the D3D9 device textures are created on. Called once the ImGui DX9
	// backend is up, which is what proves the device is usable.
	void Initialize(IDirect3DDevice9* device);

	// True when this build has a sprite for the character.
	bool IsAvailable(int charIndex);

	// Texture for `charIndex` drawn in `paletteData` (IMPL_PALETTE_DATALEN bytes of
	// BGRA, i.e. a palette's file0), sized via outWidth/outHeight. `key` identifies the
	// palette for caching and must be unique per character+palette - the palette's file
	// name is what callers use. Returns nullptr when there is no sprite, no device, or
	// the texture could not be created; callers draw their own placeholder.
	//
	// Only call while drawing: entries touched this frame are the ones kept.
	ImTextureID Get(int charIndex, const std::string& key, const char* paletteData,
		int* outWidth, int* outHeight);

	// The character's thumbnail drawn in `paletteData`, as 0xAARRGGBB pixels (transparent
	// where the sprite is not), for keeping a picture of a palette without keeping the
	// palette. False when this build has no sprite for the character.
	bool RenderPixels(int charIndex, const char* paletteData, std::vector<unsigned int>& out,
		int* outWidth, int* outHeight);

	// A texture for ready-made 0xAARRGGBB pixels, cached under `key` like the thumbnails
	// (same size cap, same rules on when to call).
	ImTextureID GetFromPixels(const std::string& key, const unsigned int* pixels, int width, int height);

	// The full reference sheet for a palette, as a texture, for the detail panel. Only
	// one is ever held: it is ~4.8 MB as RGBA, so selecting another palette releases the
	// previous one. Returns nullptr if this build has no sheet for the character.
	ImTextureID GetSheet(int charIndex, const std::string& key, const char* paletteData,
		int* outWidth, int* outHeight);

	// The reference sheet as palette indices, for the editor to map a clicked pixel back
	// to the palette entry it shows. Decoded on first use and kept, with the editor
	// texture below, until the editor stops drawing. The pointer stays valid until then.
	bool GetEditorSheetIndices(int charIndex, const unsigned char** outIndices,
		int* outWidth, int* outHeight);

	// How the editor fades the entries that are not being edited.
	struct SheetFade
	{
		unsigned char red = 90, green = 90, blue = 90; // what faded colours are pulled towards
		float strength = 0.85f; // 0 leaves them alone, 1 replaces them outright
		float shading = 0.8f;   // how much of each colour's own brightness survives, so shapes stay readable
		float opacity = 1.0f;   // alpha of a fully faded colour; the canvas shows through below 1

		bool operator==(const SheetFade& o) const
		{
			return red == o.red && green == o.green && blue == o.blue && strength == o.strength &&
				shading == o.shading && opacity == o.opacity;
		}
	};

	// The editor's live preview: the sheet drawn in `paletteData`, rebuilt only when the
	// palette, highlight or fade actually changed. `highlightMask` (256 entries, nonzero =
	// keep) leaves those entries at full colour and fades every other one per `fade`, which
	// is what shows "everything this colour paints"; nullptr fades nothing. Separate from
	// GetSheet() because the palettes window keeps drawing its own sheet behind the editor
	// in the same frame.
	//
	// Only call while drawing. Once a frame goes by without a call, the next texture request
	// frees it, since the editor has closed.
	ImTextureID GetEditorSheet(int charIndex, const char* paletteData, const unsigned char* highlightMask,
		const SheetFade& fade, int* outWidth, int* outHeight);

	// The same live preview for any indexed image, such as the effect sheets. `imageKey`
	// identifies the image (the sheet uses the character index; anything else must pick
	// keys that cannot collide with it). Shares the sheet's one texture, so only one of
	// the two can be drawn per frame.
	ImTextureID GetEditorImage(int imageKey, const unsigned char* indices, int width, int height,
		const char* paletteData, const unsigned char* highlightMask, const SheetFade& fade);

	// The same texture filled with finished 0xAARRGGBB pixels, for pictures that cannot be
	// drawn with one colour per index (the effect sheets, whose sprites each have their own
	// blend mode and colour recipe). Uploads only when `changed` or the image is new.
	ImTextureID GetEditorImageRGBA(int imageKey, const unsigned int* pixels, int width, int height, bool changed);

	// Drops a single palette's textures (thumbnail and, if it is showing, the detail
	// sheet), for when its colours change underneath us. Safe mid-frame: anything already
	// drawn this frame is released a frame later instead.
	void Invalidate(int charIndex, const std::string& key);

	// Brackets an image with nearest-neighbour sampling, so a zoomed-in sheet shows hard
	// pixels to click on instead of the backend's linear blur.
	void BeginPointSampling(ImDrawList* drawList);
	void EndPointSampling(ImDrawList* drawList);

	// Releases every texture. Must run before a D3D9 device reset - these live in
	// D3DPOOL_DEFAULT and a reset invalidates them - and again on shutdown.
	void ReleaseAll();

	// ReleaseAll plus forgetting the device, for shutdown.
	void Shutdown();
}
