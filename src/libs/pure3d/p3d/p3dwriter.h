#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal, dependency-free writer for Scrooby/Pure3D front-end .p3d files.
//
// Reverse-engineered from the Scrooby loader (FeChunkHandler.cpp) and the
// Pure3D chunk reader (chunkfile.cpp). Format summary:
//   - File root: DATA_FILE chunk (id 0xFF443350).
//   - Chunk header: [id u32][dataLength u32][chunkLength u32].
//     dataLength = offset of the sub-chunks (= 12 + payload size).
//     chunkLength = full size from this header to the end of sub-chunks.
//     Payload follows the 12-byte header; sub-chunks follow the payload.
//   - Strings are Pascal-ish: [len u8][bytes...] with NO trailing NUL.
//
// This class writes to an in-memory buffer so it stays unit-testable and is not
// tied to the game's tFile layer.
class P3DWriter
{
public:
    P3DWriter();

    // Top-level: begin the DATA_FILE wrapper and write the project header.
    void BeginProject(const char* name,
                      uint32_t screenWidth,
                      uint32_t screenHeight,
                      const char* platform,
                      const char* pagePath,
                      const char* resPath,
                      const char* screenPath);

    void BeginScreen(const char* name, uint32_t version, uint32_t numPageNames);
    // Screen references its pages by name; call one after BeginScreen for each.
    void AddScreenPageName(const char* pageName);
    void EndScreen();

    void BeginPage(const char* name, uint32_t version, uint32_t resx, uint32_t resy);
    void EndPage();

    void BeginLayer(const char* name, uint32_t version, bool visible, bool editable, uint8_t alpha);
    void EndLayer();

    void BeginGroup(const char* name, uint32_t version, uint32_t alpha);
    void EndGroup();

    // Text (MULTITEXT). version>16 adds the drop-shadow block, matching the loader.
    void WriteText(const char* name,
                   uint32_t version,
                   uint32_t x,
                   uint32_t y,
                   uint32_t dimensionX,
                   uint32_t dimensionY,
                   int justificationX,
                   int justificationY,
                   uint32_t colour,
                   int translucency,
                   float rotation,
                   const char* textStyleName,
                   bool displayShadow,
                   uint32_t shadowColour,
                   int shadowOffsetX,
                   int shadowOffsetY,
                   uint32_t index);

    // Adds a hard-coded string sub-chunk to the most recent text.
    void AddTextHardCodedString(const char* text);
    void AddTextBibleString(const char* bibleName, const char* stringId);
    // Closes the MULTITEXT chunk opened by WriteText (call after adding strings).
    void EndText();

    // Sprite (MULTISPRITE) with a set of image names.
    void WriteSprite(const char* name,
                     uint32_t version,
                     uint32_t x,
                     uint32_t y,
                     uint32_t dimensionX,
                     uint32_t dimensionY,
                     int justificationX,
                     int justificationY,
                     uint32_t colour,
                     int translucency,
                     float rotation,
                     const char* const* imageNames,
                     unsigned numImages);
    // Closes the MULTISPRITE chunk opened by WriteSprite.
    void EndSprite();

    // Polygon (FrontendPolygon 0x18009), the only front-end drawable the loader
    // supports that the writer lacked. Points are [x,y,z] floats (z ignored by
    // the loader); colours are u32 per point in surface order (ARGB).
    void WritePolygon(const char* name,
                      uint32_t version,
                      uint32_t translucency,
                      const float* points,   // 3 floats per point
                      const uint32_t* colours, // one u32 per point
                      unsigned numPoints);
    // Closes the POLYGON chunk opened by WritePolygon.
    void EndPolygon();

    void EndProject();

    // The serialized .p3d bytes.
    const std::vector<char>& GetData() const { return m_data; }

private:
    void BeginChunk(uint32_t id);
    void EndChunk();
    void WriteU8(uint8_t v);
    void WriteU32(uint32_t v);
    void WriteUInt(uint32_t v);
    void WriteInt(int v);
    void WriteFloat(float f);
    void WriteString(const char* s);

    struct ChunkFrame
    {
        size_t headerPos;    // position of the 12-byte header in m_data
        size_t payloadStart; // position just after the header
        uint32_t dataLength; // end of this chunk's payload (0 until first sub-chunk)
    };

    std::vector<char> m_data;
    std::vector<ChunkFrame> m_stack;
};