#include "p3dwriter.h"

#include <cstring>

namespace
{
    const uint32_t CHUNK_HEADER = 12; // id + dataLength + chunkLength
    const uint32_t DATA_FILE_ID = 0xFF443350;
    // Scrooby chunk ids (mirror constants/chunkids.hpp).
    enum ScroobyChunk
    {
        PROJECT     = 0x00018000,
        SCREEN      = 0x00018001,
        PAGE        = 0x00018002,
        LAYER       = 0x00018003,
        GROUP       = 0x00018004,
        MULTISPRITE = 0x00018006,
        MULTITEXT   = 0x00018007,
        STRINGTEXTBIBLE  = 0x0001800B,
        STRINGHARDCODED  = 0x0001800C,
    };
}

P3DWriter::P3DWriter()
{
}

void P3DWriter::BeginChunk(uint32_t id)
{
    ChunkFrame frame;
    frame.headerPos = m_data.size();
    frame.payloadStart = m_data.size() + CHUNK_HEADER;
    frame.dataLength = 0;
    // Reserve the 12-byte header (id + dataLength + chunkLength) as zeros.
    m_data.resize(m_data.size() + CHUNK_HEADER, 0);
    // Write the id into the first 4 bytes of the header.
    m_data[frame.headerPos + 0] = static_cast<char>(id & 0xFF);
    m_data[frame.headerPos + 1] = static_cast<char>((id >> 8) & 0xFF);
    m_data[frame.headerPos + 2] = static_cast<char>((id >> 16) & 0xFF);
    m_data[frame.headerPos + 3] = static_cast<char>((id >> 24) & 0xFF);
    m_stack.push_back(frame);

    // When we open a sub-chunk, the parent's payload ends right here, so its
    // dataLength field must point to the offset (relative to the parent's own
    // header) where the sub-chunks begin.
    if (m_stack.size() >= 2)
    {
        ChunkFrame& parent = m_stack[m_stack.size() - 2];
        if (parent.dataLength == 0)
        {
            parent.dataLength = static_cast<uint32_t>(frame.headerPos - parent.headerPos);
        }
    }
}

void P3DWriter::EndChunk()
{
    ChunkFrame frame = m_stack.back();
    m_stack.pop_back();

    uint32_t totalEnd = static_cast<uint32_t>(m_data.size());
    uint32_t chunkLength = totalEnd - frame.headerPos;
    // For a leaf chunk (no sub-chunks), dataLength == chunkLength. For a chunk
    // with sub-chunks, dataLength was fixed in BeginChunk of the first sub-chunk.
    uint32_t dataLength = frame.dataLength != 0 ? frame.dataLength : chunkLength;

    // Patch the dataLength and chunkLength fields at headerPos+4 and headerPos+8.
    char* base = &m_data[frame.headerPos];
    base[4] = static_cast<char>(dataLength & 0xFF);
    base[5] = static_cast<char>((dataLength >> 8) & 0xFF);
    base[6] = static_cast<char>((dataLength >> 16) & 0xFF);
    base[7] = static_cast<char>((dataLength >> 24) & 0xFF);
    base[8] = static_cast<char>(chunkLength & 0xFF);
    base[9] = static_cast<char>((chunkLength >> 8) & 0xFF);
    base[10] = static_cast<char>((chunkLength >> 16) & 0xFF);
    base[11] = static_cast<char>((chunkLength >> 24) & 0xFF);
}

void P3DWriter::WriteU8(uint8_t v)
{
    m_data.push_back(static_cast<char>(v));
}

void P3DWriter::WriteU32(uint32_t v)
{
    m_data.push_back(static_cast<char>(v & 0xFF));
    m_data.push_back(static_cast<char>((v >> 8) & 0xFF));
    m_data.push_back(static_cast<char>((v >> 16) & 0xFF));
    m_data.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void P3DWriter::WriteUInt(uint32_t v) { WriteU32(v); }
void P3DWriter::WriteInt(int v) { WriteU32(static_cast<uint32_t>(v)); }

void P3DWriter::WriteFloat(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(uint32_t));
    WriteU32(bits);
}

void P3DWriter::WriteString(const char* s)
{
    size_t len = s ? std::strlen(s) : 0;
    if (len > 255) len = 255;
    WriteU8(static_cast<uint8_t>(len));
    m_data.insert(m_data.end(), s, s + len);
}

void P3DWriter::BeginProject(const char* name,
                             uint32_t screenWidth,
                             uint32_t screenHeight,
                             const char* platform,
                             const char* pagePath,
                             const char* resPath,
                             const char* screenPath)
{
    BeginChunk(DATA_FILE_ID);
    BeginChunk(PROJECT);
    WriteString(name);
    WriteU32(1); // version
    WriteU32(screenWidth);
    WriteU32(screenHeight);
    WriteString(platform);
    WriteString(pagePath);
    WriteString(resPath);
    WriteString(screenPath);
}

void P3DWriter::BeginScreen(const char* name, uint32_t version, uint32_t numPageNames)
{
    BeginChunk(SCREEN);
    WriteString(name);
    WriteU32(version);
    WriteU32(numPageNames);
}

void P3DWriter::AddScreenPageName(const char* pageName)
{
    WriteString(pageName);
}

void P3DWriter::EndScreen()
{
    EndChunk();
}

void P3DWriter::BeginPage(const char* name, uint32_t version, uint32_t resx, uint32_t resy)
{
    BeginChunk(PAGE);
    WriteString(name);
    WriteU32(version);
    WriteU32(resx);
    WriteU32(resy);
}

void P3DWriter::EndPage()
{
    EndChunk();
}

void P3DWriter::BeginLayer(const char* name, uint32_t version, bool visible, bool editable, uint8_t alpha)
{
    BeginChunk(LAYER);
    WriteString(name);
    WriteU32(version);
    WriteU32(visible ? 1 : 0);
    WriteU32(editable ? 1 : 0);
    WriteUInt(alpha);
}

void P3DWriter::EndLayer()
{
    EndChunk();
}

void P3DWriter::BeginGroup(const char* name, uint32_t version)
{
    BeginChunk(GROUP);
    WriteString(name);
    WriteU32(version);
}

void P3DWriter::EndGroup()
{
    EndChunk();
}

void P3DWriter::WriteText(const char* name,
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
                          uint32_t index)
{
    BeginChunk(MULTITEXT);
    WriteString(name);
    WriteU32(version);
    WriteU32(x);
    WriteU32(y);
    WriteU32(dimensionX);
    WriteU32(dimensionY);
    WriteInt(justificationX);
    WriteInt(justificationY);
    WriteUInt(colour);
    WriteInt(translucency);
    WriteFloat(rotation);
    WriteString(textStyleName);
    if (version > 16)
    {
        WriteU8(displayShadow ? 1 : 0);
        WriteUInt(shadowColour);
        WriteInt(shadowOffsetX);
        WriteInt(shadowOffsetY);
        WriteUInt(index);
    }
}

void P3DWriter::AddTextHardCodedString(const char* text)
{
    BeginChunk(STRINGHARDCODED);
    WriteString(text);
    EndChunk();
}

void P3DWriter::AddTextBibleString(const char* bibleName, const char* stringId)
{
    BeginChunk(STRINGTEXTBIBLE);
    WriteString(bibleName);
    WriteString(stringId);
    EndChunk();
}

void P3DWriter::EndText()
{
    EndChunk();
}

void P3DWriter::WriteSprite(const char* name,
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
                            unsigned numImages)
{
    BeginChunk(MULTISPRITE);
    WriteString(name);
    WriteU32(version);
    WriteU32(x);
    WriteU32(y);
    WriteU32(dimensionX);
    WriteU32(dimensionY);
    WriteInt(justificationX);
    WriteInt(justificationY);
    WriteUInt(colour);
    WriteInt(translucency);
    WriteFloat(rotation);
    WriteU32(numImages);
    for (unsigned i = 0; i < numImages; ++i)
    {
        WriteString(imageNames[i]);
    }
}

void P3DWriter::EndSprite()
{
    EndChunk();
}

void P3DWriter::EndProject()
{
    // Close the PROJECT chunk then the DATA_FILE wrapper.
    EndChunk();
    EndChunk();
}