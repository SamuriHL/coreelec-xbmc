/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEStreamInfo.h"

#include "utils/log.h"

#include <algorithm>
#include <string.h>

#define DTS_PREAMBLE_14BE 0x1FFFE800
#define DTS_PREAMBLE_14LE 0xFF1F00E8
#define DTS_PREAMBLE_16BE 0x7FFE8001
#define DTS_PREAMBLE_16LE 0xFE7F0180
#define DTS_PREAMBLE_HD 0x64582025
#define DTS_PREAMBLE_XCH 0x5a5a5a5a
#define DTS_PREAMBLE_XXCH 0x47004a03
#define DTS_PREAMBLE_X96K 0x1d95f262
#define DTS_PREAMBLE_XBR 0x655e315e
#define DTS_PREAMBLE_LBR 0x0a801921
#define DTS_PREAMBLE_XLL 0x41a29547
#define DTS_SFREQ_COUNT 16
#define MAX_EAC3_BLOCKS 6
#define UNKNOWN_DTS_EXTENSION 255

static const uint16_t AC3Bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                       192, 224, 256, 320, 384, 448, 512, 576, 640};
static const uint16_t AC3FSCod[] = {48000, 44100, 32000, 0};
static const uint8_t AC3BlkCod[] = {1, 2, 3, 6};
static const uint8_t AC3Channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
static const uint8_t DTSChannels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
static const uint8_t THDChanMap[] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};

static const uint32_t DTSSampleRates[DTS_SFREQ_COUNT] = {0,     8000,  16000, 32000, 64000,  128000,
                                                         11025, 22050, 44100, 88200, 176400, 12000,
                                                         24000, 48000, 96000, 192000};

CAEStreamParser::CAEStreamParser() : m_syncFunc(&CAEStreamParser::DetectType)
{
  av_crc_init(m_crcTrueHD, 0, 16, 0x2D, sizeof(m_crcTrueHD));
}

double CAEStreamInfo::GetDuration() const
{
  double duration = 0;
  switch (m_type)
  {
    case STREAM_TYPE_AC3:
      duration = 1536.0 / m_sampleRate;
      break;
    case STREAM_TYPE_EAC3:
      duration = 6144.0 / m_sampleRate / 4;
      break;
    case STREAM_TYPE_TRUEHD:
      int rate;
      if (m_sampleRate == 48000 || m_sampleRate == 96000 || m_sampleRate == 192000)
        rate = 192000;
      else
        rate = 176400;
      duration = 3840.0 / rate;
      break;
    case STREAM_TYPE_DTS_512:
    case STREAM_TYPE_DTSHD_CORE:
    case STREAM_TYPE_DTSHD:
    case STREAM_TYPE_DTSHD_MA:
      duration = 512.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_1024:
      duration = 1024.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_2048:
      duration = 2048.0 / m_sampleRate;
      break;
    default:
      CLog::Log(LOGERROR, "CAEStreamInfo::GetDuration - invalid stream type");
      break;
  }
  return duration * 1000;
}

bool CAEStreamInfo::operator==(const CAEStreamInfo& info) const
{
  if (m_type != info.m_type)
    return false;
  if (m_dataIsLE != info.m_dataIsLE)
    return false;
  if (m_repeat != info.m_repeat)
    return false;
  return true;
}

void CAEStreamParser::Reset()
{
  m_skipBytes = 0;
  m_bufferSize = 0;
  m_needBytes = 0;
  m_hasSync = false;
}

int CAEStreamParser::AddData(uint8_t* data,
                             unsigned int size,
                             uint8_t** buffer,
                             unsigned int* bufferSize)
{
  if (size == 0)
  {
    if (bufferSize)
      *bufferSize = 0;
    return 0;
  }

  if (m_skipBytes)
  {
    unsigned int canSkip = std::min(size, m_skipBytes);
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    unsigned int copy = std::min(room, canSkip);

    memcpy(m_buffer + m_bufferSize, data, copy);
    m_bufferSize += copy;
    m_skipBytes -= copy;

    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return copy;
    }

    GetPacket(buffer, bufferSize);
    return copy;
  }
  else
  {
    unsigned int consumed = 0;
    unsigned int offset = 0;
    unsigned int room = sizeof(m_buffer) - m_bufferSize;
    while (true)
    {
      if (!size)
      {
        if (bufferSize)
          *bufferSize = 0;
        return consumed;
      }

      unsigned int copy = std::min(room, size);
      memcpy(m_buffer + m_bufferSize, data, copy);
      m_bufferSize += copy;
      consumed += copy;
      data += copy;
      size -= copy;
      room -= copy;

      if (m_needBytes > m_bufferSize)
        continue;

      m_needBytes = 0;
      offset = (this->*m_syncFunc)(m_buffer, m_bufferSize);

      if (m_hasSync)
        break;
      else
      {
        // lost sync
        m_syncFunc = &CAEStreamParser::DetectType;
        m_info.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
        m_info.m_repeat = 1;

        // if the buffer is full, or the offset < the buffer size
        if (m_bufferSize == sizeof(m_buffer) || offset < m_bufferSize)
        {
          m_bufferSize -= offset;
          room += offset;
          memmove(m_buffer, m_buffer + offset, m_bufferSize);
        }
      }
    }

    // if we got here, we acquired sync on the buffer

    // align the buffer
    if (offset)
    {
      m_bufferSize -= offset;
      memmove(m_buffer, m_buffer + offset, m_bufferSize);
    }

    // bytes to skip until the next packet
    m_skipBytes = std::max(0, (int)m_fsize - (int)m_bufferSize);
    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return consumed;
    }

    if (!m_needBytes)
      GetPacket(buffer, bufferSize);
    else if (bufferSize)
      *bufferSize = 0;

    return consumed;
  }
}

namespace
{
// Bit access for the fields rewritten by the dialogue normalisation defeat.
uint32_t ReadBits(const uint8_t* d, unsigned int& pos, unsigned int n)
{
  uint32_t v = 0;
  while (n--)
  {
    v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
    ++pos;
  }
  return v;
}

void WriteBits(uint8_t* d, unsigned int pos, unsigned int n, uint32_t value)
{
  for (unsigned int i = 0; i < n; ++i)
  {
    const uint8_t mask = 1 << (7 - ((pos + i) & 7));
    if ((value >> (n - 1 - i)) & 1)
      d[(pos + i) >> 3] |= mask;
    else
      d[(pos + i) >> 3] &= ~mask;
  }
}

unsigned int PopCount(uint32_t x)
{
  unsigned int c = 0;
  for (; x; x >>= 1)
    c += x & 1;
  return c;
}

// Dolby dialnorm (AC-3, E-AC-3, TrueHD): code 1..31 (1..63 for TrueHD 2ch) is the
// dialogue level in -dB/LKFS, gain = code - 31; the reserved 0 means -31 = no gain.
int DolbyDialNormGain(unsigned int code)
{
  return code == 0 ? 0 : static_cast<int>(code) - 31;
}

// AC-3 CRC-1 recomputation (ATSC A/52 5.4.1.4), as libavcodec/ac3enc.c does it.
constexpr unsigned int AC3_CRC16_POLY = (1 << 0) | (1 << 2) | (1 << 15) | (1 << 16);

unsigned int AC3MulPoly(unsigned int a, unsigned int b, unsigned int poly)
{
  unsigned int c = 0;
  while (a)
  {
    if (a & 1)
      c ^= b;
    a >>= 1;
    b <<= 1;
    if (b & (1 << 16))
      b ^= poly;
  }
  return c;
}

unsigned int AC3PowPoly(unsigned int a, unsigned int n, unsigned int poly)
{
  unsigned int r = 1;
  while (n)
  {
    if (n & 1)
      r = AC3MulPoly(r, a, poly);
    a = AC3MulPoly(a, a, poly);
    n >>= 1;
  }
  return r;
}

uint16_t Bswap16(uint16_t x)
{
  return static_cast<uint16_t>((x >> 8) | (x << 8));
}

// DTS-HD extension substream header CRC (ETSI TS 102 114 7.5.2)
uint16_t CRC16CCITT(const uint8_t* d, unsigned int len)
{
  uint16_t crc = 0xFFFF;
  for (unsigned int i = 0; i < len; ++i)
  {
    crc ^= static_cast<uint16_t>(d[i]) << 8;
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}
} // namespace

void CAEStreamParser::ProcessAC3DialNorm(uint8_t* data, unsigned int size)
{
  const AVCRC* crcTable = av_crc_get_table(AV_CRC_16_ANSI);
  bool reported = false;
  unsigned int offset = 0;

  // m_buffer holds one independent frame, possibly followed by E-AC-3 dependent frames
  while (offset + 8 <= size)
  {
    uint8_t* frame = data + offset;
    if (frame[0] != 0x0B || frame[1] != 0x77)
      break;

    const uint8_t bsid = frame[5] >> 3;
    unsigned int frameBytes = 0;
    unsigned int dnPos = 0; // bit position of the 5-bit dialnorm
    bool independent = true;

    if (bsid <= 10)
    {
      const uint8_t fscod = frame[4] >> 6;
      const uint8_t frmsizecod = frame[4] & 0x3F;
      if (fscod == 3 || frmsizecod > 37)
        break;

      const unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
      unsigned int framewords = 0;
      switch (fscod)
      {
        case 0:
          framewords = bitRate * 2;
          break;
        case 1:
          framewords = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0));
          break;
        case 2:
          framewords = bitRate * 4;
          break;
      }
      frameBytes = framewords * 2;

      // bsi: bsid(5) bsmod(3) acmod(3) [cmixlev(2)] [surmixlev(2)] [dsurmod(2)] lfeon(1) dialnorm(5)
      const uint8_t acmod = frame[6] >> 5;
      dnPos = 6 * 8 + 3;
      if ((acmod & 0x1) && acmod != 0x1)
        dnPos += 2;
      if (acmod & 0x4)
        dnPos += 2;
      if (acmod == 0x2)
        dnPos += 2;
      dnPos += 1;
    }
    else if (bsid <= 16)
    {
      frameBytes = ((((frame[2] & 0x7) << 8) | frame[3]) + 1) * 2;
      independent = (frame[2] >> 6) != 1;
      // bsi: strmtyp(2) substreamid(3) frmsiz(11) fscod(2) numblkscod(2) acmod(3) lfeon(1) bsid(5) dialnorm(5)
      dnPos = 5 * 8 + 5;
    }
    else
      break;

    if (frameBytes < 16 || offset + frameBytes > size)
      break;

    unsigned int pos = dnPos;
    const unsigned int code = ReadBits(frame, pos, 5);
    const bool defeat = m_defeatAC3DialNorm && code != 31 && code != 0;

    if (independent && !reported)
    {
      m_info.m_hasDialNorm = true;
      m_info.m_dialNorm = DolbyDialNormGain(code);
      m_info.m_dialNormApplied = defeat ? 0 : m_info.m_dialNorm;
      reported = true;
    }

    if (defeat)
    {
      WriteBits(frame, dnPos, 5, 31);

      if (bsid <= 10)
      {
        // CRC-1 makes the first 5/8 of the frame check to zero, so re-deriving
        // it keeps CRC-2 (whole frame) valid without touching it.
        const unsigned int size58 = ((frameBytes >> 2) + (frameBytes >> 4)) << 1;
        uint16_t crc1 =
            Bswap16(static_cast<uint16_t>(av_crc(crcTable, 0, frame + 4, size58 - 4)));
        const unsigned int crcInv =
            AC3PowPoly(AC3_CRC16_POLY >> 1, (8 * size58) - 16, AC3_CRC16_POLY);
        crc1 = static_cast<uint16_t>(AC3MulPoly(crcInv, crc1, AC3_CRC16_POLY));
        frame[2] = crc1 >> 8;
        frame[3] = crc1 & 0xFF;
      }
      else
      {
        uint16_t crc2 =
            Bswap16(static_cast<uint16_t>(av_crc(crcTable, 0, frame + 2, frameBytes - 4)));
        if (crc2 == 0x0B77)
        {
          // avoid emulating a sync word: flip crcrsv, as the encoder does
          frame[frameBytes - 3] ^= 0x1;
          crc2 ^= 0x8005;
        }
        frame[frameBytes - 2] = crc2 >> 8;
        frame[frameBytes - 1] = crc2 & 0xFF;
      }
    }

    offset += frameBytes;
  }
}

void CAEStreamParser::ProcessDTSDialNorm(uint8_t* data, unsigned int size)
{
  if (size < 16)
    return;

  const uint32_t sync = (static_cast<uint32_t>(data[0]) << 24) |
                        (static_cast<uint32_t>(data[1]) << 16) |
                        (static_cast<uint32_t>(data[2]) << 8) | data[3];
  if (sync != DTS_PREAMBLE_16BE)
    return;

  // Core frame header (ETSI TS 102 114 5.3.1)
  unsigned int pos = 32;
  ReadBits(data, pos, 1 + 5); // FTYPE, SHORT
  const unsigned int cpf = ReadBits(data, pos, 1);
  ReadBits(data, pos, 7); // NBLKS
  const unsigned int fsize = ReadBits(data, pos, 14) + 1;
  ReadBits(data, pos, 6 + 4 + 5); // AMODE, SFREQ, RATE
  ReadBits(data, pos, 1 + 1 + 1 + 1 + 1); // FixedBit, DYNF, TIMEF, AUXF, HDCD
  ReadBits(data, pos, 3 + 1 + 1 + 2 + 1); // EXT_AUDIO_ID, EXT_AUDIO, ASPF, LFF, HFLAG
  if (cpf)
    ReadBits(data, pos, 16); // HCRC
  ReadBits(data, pos, 1); // FILTS
  const unsigned int vernumPos = pos;
  const unsigned int vernum = ReadBits(data, pos, 4);
  ReadBits(data, pos, 2 + 3 + 1 + 1); // CHIST, PCMR, SUMF, SUMS
  const unsigned int dialnormPos = pos;
  const unsigned int dialnorm = ReadBits(data, pos, 4);

  // DNG = -(16 + DIALNORM) for VERNUM 6, -DIALNORM for VERNUM 7, else none (5.3.1)
  int coreGain = 0;
  if (vernum == 7)
    coreGain = -static_cast<int>(dialnorm);
  else if (vernum == 6)
    coreGain = -16 - static_cast<int>(dialnorm);

  int reportGain = coreGain;
  bool defeated = false;

  // The header CRC (CPF) would have to be recomputed too; leave those frames alone.
  if (m_defeatDTSDialNorm && !cpf && coreGain != 0)
  {
    // VERNUM 6 cannot express 0 dB; 7 is the same compatible revision where it can
    if (vernum == 6)
      WriteBits(data, vernumPos, 4, 7);
    WriteBits(data, dialnormPos, 4, 0);
    defeated = true;
  }

  // DTS-HD extension substream: the first asset's dialnorm code (7.5)
  if (fsize + 16 <= size)
  {
    const unsigned int exss = fsize;
    const uint32_t exssSync = (static_cast<uint32_t>(data[exss]) << 24) |
                              (static_cast<uint32_t>(data[exss + 1]) << 16) |
                              (static_cast<uint32_t>(data[exss + 2]) << 8) | data[exss + 3];
    if (exssSync == DTS_PREAMBLE_HD)
    {
      uint8_t* eb = data + exss;
      unsigned int ep = 32;
      ReadBits(eb, ep, 8); // UserDefinedBits
      const unsigned int nExtSSIndex = ReadBits(eb, ep, 2);
      const unsigned int headerSizeType = ReadBits(eb, ep, 1);
      const unsigned int bitsHeader = headerSizeType ? 12 : 8;
      const unsigned int bitsFsize = headerSizeType ? 20 : 16;
      const unsigned int headerSize = ReadBits(eb, ep, bitsHeader) + 1;

      if (headerSize >= 7 && exss + headerSize <= size &&
          CRC16CCITT(eb + 5, headerSize - 5) == 0)
      {
        int assetGain = 0;
        unsigned int dnPos = 0;
        bool haveAsset = false;

        do
        {
          ReadBits(eb, ep, bitsFsize);
          const unsigned int staticFields = ReadBits(eb, ep, 1);
          unsigned int numAssets = 1;
          if (staticFields)
          {
            ReadBits(eb, ep, 2 + 3); // RefClockCode, ExSSFrameDurationCode
            if (ReadBits(eb, ep, 1)) // TimeStampFlag
              ReadBits(eb, ep, 32 + 4);
            const unsigned int numPresentations = ReadBits(eb, ep, 3) + 1;
            numAssets = ReadBits(eb, ep, 3) + 1;
            uint32_t masks[8] = {0};
            for (unsigned int i = 0; i < numPresentations; ++i)
              masks[i] = ReadBits(eb, ep, nExtSSIndex + 1);
            for (unsigned int i = 0; i < numPresentations; ++i)
              for (unsigned int ss = 0; ss < nExtSSIndex + 1u; ++ss)
                if ((masks[i] >> ss) & 1)
                  ReadBits(eb, ep, 8);
            if (ReadBits(eb, ep, 1)) // MixMetadataEnbl: not walked
              break;
          }
          for (unsigned int i = 0; i < numAssets; ++i)
            ReadBits(eb, ep, bitsFsize);

          // AssetDescriptor()
          const unsigned int descrEnd = ep + (ReadBits(eb, ep, 9) + 1) * 8;
          if (descrEnd > (headerSize - 2) * 8)
            break;
          ReadBits(eb, ep, 3); // AssetIndex
          if (staticFields)
          {
            if (ReadBits(eb, ep, 1)) // AssetTypeDescrPresent
              ReadBits(eb, ep, 4);
            if (ReadBits(eb, ep, 1)) // LanguageDescrPresent
              ReadBits(eb, ep, 24);
            if (ReadBits(eb, ep, 1)) // InfoTextPresent
              ep += (ReadBits(eb, ep, 10) + 1) * 8;
            ReadBits(eb, ep, 5 + 4); // BitResolution, MaxSampleRate
            const unsigned int totalChannels = ReadBits(eb, ep, 8) + 1;
            if (ReadBits(eb, ep, 1)) // One2OneMapChannels2Speakers
            {
              if (totalChannels > 2)
                ReadBits(eb, ep, 1);
              if (totalChannels > 6)
                ReadBits(eb, ep, 1);
              unsigned int maskBits = 0;
              if (ReadBits(eb, ep, 1)) // SpkrMaskEnabled
              {
                maskBits = (ReadBits(eb, ep, 2) + 1) * 4;
                ReadBits(eb, ep, maskBits);
              }
              const unsigned int numRemaps = ReadBits(eb, ep, 3);
              uint32_t layouts[8] = {0};
              for (unsigned int i = 0; i < numRemaps; ++i)
                layouts[i] = ReadBits(eb, ep, maskBits);
              for (unsigned int i = 0; i < numRemaps; ++i)
              {
                const unsigned int numDecCh = ReadBits(eb, ep, 5) + 1;
                const unsigned int numSpkrs = PopCount(layouts[i]);
                for (unsigned int c = 0; c < numSpkrs; ++c)
                {
                  const unsigned int coeffs = PopCount(ReadBits(eb, ep, numDecCh));
                  ReadBits(eb, ep, coeffs * 5);
                }
              }
            }
            else
              ReadBits(eb, ep, 3); // RepresentationType
          }
          if (ReadBits(eb, ep, 1)) // DRCCoefPresent
            ReadBits(eb, ep, 8);
          if (!ReadBits(eb, ep, 1)) // DialNormPresent
            break;
          dnPos = ep;
          if (dnPos + 5 > descrEnd)
            break;
          assetGain = -static_cast<int>(ReadBits(eb, ep, 5));
          haveAsset = true;
        } while (false);

        if (haveAsset)
        {
          reportGain = assetGain;
          if (m_defeatDTSDialNorm && assetGain != 0)
          {
            WriteBits(eb, dnPos, 5, 0);
            const uint16_t crc = CRC16CCITT(eb + 5, headerSize - 2 - 5);
            eb[headerSize - 2] = crc >> 8;
            eb[headerSize - 1] = crc & 0xFF;
            defeated = true;
          }
        }
      }
    }
  }

  m_info.m_hasDialNorm = true;
  m_info.m_dialNorm = reportGain;
  m_info.m_dialNormApplied = defeated ? 0 : reportGain;
}

void CAEStreamParser::ProcessTrueHDDialNorm(uint8_t* data, unsigned int majorSyncSize)
{
  // data points at the access unit; major_sync_info starts at data[4]. channel_meaning()
  // starts at data[22] (Dolby TrueHD high-level bitstream description 4.3):
  // reserved(6) 2ch/6ch/8ch_control_enabled(3) reserved(1) drc_start_up_gain(7)
  // 2ch_dialogue_norm(6) 2ch_mix_level(6) 6ch_dialogue_norm(5) 6ch_mix_level(6)
  // 6ch_source_format(5) 8ch_dialogue_norm(5) ...
  struct Field
  {
    unsigned int pos;
    unsigned int bits;
  };
  const Field ch2{22 * 8 + 17, 6};
  const Field ch6{22 * 8 + 29, 5};
  const Field ch8{22 * 8 + 45, 5};
  // 16ch_channel_meaning() follows extra_channel_meaning_length(4) at data[30] (4.4)
  const bool has16ch = (data[29] & 0x01) && (data[21] & 0x80) && majorSyncSize >= 32;
  const Field ch16{30 * 8 + 4, 5};

  const auto read = [data](const Field& f)
  {
    unsigned int pos = f.pos;
    return ReadBits(data, pos, f.bits);
  };

  // report the widest presentation the stream carries
  unsigned int reportCode;
  if (has16ch)
    reportCode = read(ch16);
  else if ((data[21] >> 4) & 0x7)
    reportCode = read(ch8);
  else if ((data[21] >> 2) & 0x3)
    reportCode = read(ch6);
  else
    reportCode = read(ch2);

  bool modified = false;
  if (m_defeatTrueHDDialNorm)
  {
    for (const Field* f : {&ch2, &ch6, &ch8, has16ch ? &ch16 : nullptr})
    {
      if (!f)
        continue;
      const unsigned int code = read(*f);
      if (code != 31 && code != 0)
      {
        WriteBits(data, f->pos, f->bits, 31);
        modified = true;
      }
    }
  }

  if (modified)
  {
    uint16_t crc = av_crc(m_crcTrueHD, 0, data + 4, majorSyncSize - 4);
    crc ^= (data[4 + majorSyncSize - 3] << 8) | data[4 + majorSyncSize - 4];
    data[4 + majorSyncSize - 2] = crc & 0xFF;
    data[4 + majorSyncSize - 1] = crc >> 8;
  }

  m_info.m_hasDialNorm = true;
  m_info.m_dialNorm = DolbyDialNormGain(reportCode);
  m_info.m_dialNormApplied =
      m_defeatTrueHDDialNorm && reportCode != 0 ? 0 : m_info.m_dialNorm;
}

void CAEStreamParser::GetPacket(uint8_t** buffer, unsigned int* bufferSize)
{
  // if the caller wants the packet
  if (buffer)
  {
    // if it is dtsHD and we only want the core, just fetch that
    unsigned int size = m_fsize;
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE)
      size = m_coreSize;

    switch (m_info.m_type)
    {
      case CAEStreamInfo::STREAM_TYPE_AC3:
      case CAEStreamInfo::STREAM_TYPE_EAC3:
        ProcessAC3DialNorm(m_buffer, size);
        break;
      case CAEStreamInfo::STREAM_TYPE_DTS_512:
      case CAEStreamInfo::STREAM_TYPE_DTS_1024:
      case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      case CAEStreamInfo::STREAM_TYPE_DTSHD:
      case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
        ProcessDTSDialNorm(m_buffer, size);
        break;
      default:
        break;
    }

    // make sure the buffer is allocated and big enough
    if (!*buffer || !bufferSize || *bufferSize < size)
    {
      delete[] * buffer;
      *buffer = new uint8_t[size];
    }

    // copy the data into the buffer and update the size
    memcpy(*buffer, m_buffer, size);
    if (bufferSize)
      *bufferSize = size;
  }

  // remove the parsed data from the buffer
  m_bufferSize -= m_fsize;
  memmove(m_buffer, m_buffer + m_fsize, m_bufferSize);
  m_fsize = 0;
  m_coreSize = 0;
}

// SYNC FUNCTIONS

// This function looks for sync words across the types in parallel, and only does an exhaustive
// test if it finds a syncword. Once sync has been established, the relevant sync function sets
// m_syncFunc to itself. This function will only be called again if total sync is lost, which
// allows is to switch stream types on the fly much like a real receiver does.
unsigned int CAEStreamParser::DetectType(uint8_t* data, unsigned int size)
{
  unsigned int skipped = 0;
  unsigned int possible = 0;

  while (size > 8)
  {
    // if it could be DTS
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    if (header == DTS_PREAMBLE_14LE || header == DTS_PREAMBLE_14BE || header == DTS_PREAMBLE_16LE ||
        header == DTS_PREAMBLE_16BE)
    {
      unsigned int skip = SyncDTS(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be AC3
    if (data[0] == 0x0b && data[1] == 0x77)
    {
      unsigned int skip = SyncAC3(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      else
        possible = skipped;
    }

    // if it could be TrueHD
    if (data[4] == 0xf8 && data[5] == 0x72 && data[6] == 0x6f && data[7] == 0xba)
    {
      unsigned int skip = SyncTrueHD(data, size);
      if (m_hasSync)
        return skipped + skip;
      else
        possible = skipped;
    }

    // move along one byte
    --size;
    ++skipped;
    ++data;
  }

  return possible ? possible : skipped;
}

bool CAEStreamParser::TrySyncAC3(uint8_t* data,
                                 unsigned int size,
                                 bool resyncing,
                                 bool wantEAC3dependent)
{
  if (size < 8)
    return false;

  // look for an ac3 sync word
  if (data[0] != 0x0b || data[1] != 0x77)
    return false;

  uint8_t bsid = data[5] >> 3;
  uint8_t acmod = data[6] >> 5;
  uint8_t lfeon;

  int8_t pos = 4;
  if ((acmod & 0x1) && (acmod != 0x1))
    pos -= 2;
  if (acmod & 0x4)
    pos -= 2;
  if (acmod == 0x2)
    pos -= 2;
  if (pos < 0)
    lfeon = (data[7] & 0x64) ? 1 : 0;
  else
    lfeon = ((data[6] >> pos) & 0x1) ? 1 : 0;

  if (bsid > 0x11 || acmod > 7)
    return false;

  if (bsid <= 10)
  {
    // Normal AC-3

    if (wantEAC3dependent)
      return false;

    uint8_t fscod = data[4] >> 6;
    uint8_t frmsizecod = data[4] & 0x3F;
    if (fscod == 3 || frmsizecod > 37)
      return false;

    // get the details we need to check crc1 and framesize
    unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch (fscod)
    {
      case 0:
        framesize = bitRate * 2;
        break;
      case 1:
        framesize = (320 * bitRate / 147 + (frmsizecod & 1 ? 1 : 0));
        break;
      case 2:
        framesize = bitRate * 4;
        break;
    }

    m_fsize = framesize << 1;
    m_info.m_sampleRate = AC3FSCod[fscod];

    // dont do extensive testing if we have not lost sync
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !resyncing)
      return true;

    // this may be the main stream of EAC3
    unsigned int fsizeMain = m_fsize;
    unsigned int reqBytes = fsizeMain + 8;
    if (size < reqBytes)
    {
      // not enough data to check for E-AC3 dependent frame, request more
      m_needBytes = reqBytes;
      m_fsize = 0;
      // no need to resync => return true
      return true;
    }
    m_info.m_frameSize = fsizeMain;
    if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
    {
      // concatenate the main and dependent frames
      m_fsize += fsizeMain;
      return true;
    }

    unsigned int crc_size;
    // if we have enough data, validate the entire packet, else try to validate crc2 (5/8 of the packet)
    if (framesize <= size)
      crc_size = framesize - 1;
    else
      crc_size = (framesize >> 1) + (framesize >> 3) - 1;

    if (crc_size <= size)
      if (av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, &data[2], crc_size * 2))
        return false;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
    m_info.m_frameSize += m_fsize;
    m_info.m_repeat = 1;

    CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - AC3 stream detected ({} channels, {}Hz)",
              m_info.m_channels, m_info.m_sampleRate);
    return true;
  }
  else
  {
    // Enhanced AC-3
    uint8_t strmtyp = data[2] >> 6;
    if (strmtyp == 3)
      return false;

    if (strmtyp != 1 && wantEAC3dependent)
      return false;

    unsigned int framesize = (((data[2] & 0x7) << 8) | data[3]) + 1;
    uint8_t fscod = (data[4] >> 6) & 0x3;
    uint8_t cod = (data[4] >> 4) & 0x3;
    uint8_t acmod = (data[4] >> 1) & 0x7;
    uint8_t lfeon = data[4] & 0x1;
    uint8_t blocks;

    if (fscod == 0x3)
    {
      if (cod == 0x3)
        return false;

      blocks = 6;
      m_info.m_sampleRate = AC3FSCod[cod] >> 1;
    }
    else
    {
      blocks = AC3BlkCod[cod];
      m_info.m_sampleRate = AC3FSCod[fscod];
    }

    m_fsize = framesize << 1;
    m_info.m_repeat = MAX_EAC3_BLOCKS / blocks;

    // EAC3 can have a dependent stream too
    if (!wantEAC3dependent)
    {
      unsigned int fsizeMain = m_fsize;
      unsigned int reqBytes = fsizeMain + 8;
      if (size < reqBytes)
      {
        // not enough data to check for E-AC3 dependent frame, request more
        m_needBytes = reqBytes;
        m_fsize = 0;
        // no need to resync => return true
        return true;
      }
      m_info.m_frameSize = fsizeMain;
      if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
      {
        // concatenate the main and dependent frames
        m_fsize += fsizeMain;
        return true;
      }
    }

    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && m_hasSync && !resyncing)
      return true;

    // if we get here, we can sync
    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
    m_info.m_frameSize += m_fsize;

    CLog::Log(LOGINFO, "CAEStreamParser::TrySyncAC3 - E-AC3 stream detected ({} channels, {}Hz)",
              m_info.m_channels, m_info.m_sampleRate);
    return true;
  }
}

unsigned int CAEStreamParser::SyncAC3(uint8_t* data, unsigned int size)
{
  unsigned int skip = 0;

  for (; size - skip > 7; ++skip, ++data)
  {
    bool resyncing = (skip != 0);
    if (TrySyncAC3(data, size - skip, resyncing, false))
      return skip;
  }

  // if we get here, the entire packet is invalid and we have lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncAC3 - AC3 sync lost");
  m_hasSync = false;
  return skip;
}

unsigned int CAEStreamParser::SyncDTS(uint8_t* data, unsigned int size)
{
  if (size < 13)
  {
    if (m_needBytes < 13)
      m_needBytes = 14;
    return 0;
  }

  unsigned int skip = 0;
  for (; size - skip > 13; ++skip, ++data)
  {
    unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    unsigned int hd_sync = 0;
    unsigned int dtsBlocks;
    unsigned int amode;
    unsigned int sfreq;
    unsigned int target_rate;
    unsigned int extension = 0;
    unsigned int ext_type = UNKNOWN_DTS_EXTENSION;
    unsigned int lfe;
    int bits;

    switch (header)
    {
      // 14bit BE
      case DTS_PREAMBLE_14BE:
        if (data[4] != 0x07 || (data[5] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[5] & 0x7) << 4) | ((data[6] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[6] & 0x3) << 8) | data[7]) << 4) | ((data[8] & 0x3C) >> 2)) + 1;
        amode = ((data[8] & 0x3) << 4) | ((data[9] & 0xF0) >> 4);
        target_rate = ((data[10] & 0x3e) >> 1);
        extension = ((data[11] & 0x1));
        ext_type = ((data[11] & 0xe) >> 1);
        sfreq = data[9] & 0xF;
        lfe = (data[12] & 0x18) >> 3;
        m_info.m_dataIsLE = false;
        bits = 14;
        break;

      // 14bit LE
      case DTS_PREAMBLE_14LE:
        if (data[5] != 0x07 || (data[4] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[4] & 0x7) << 4) | ((data[7] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[7] & 0x3) << 8) | data[6]) << 4) | ((data[9] & 0x3C) >> 2)) + 1;
        amode = ((data[9] & 0x3) << 4) | ((data[8] & 0xF0) >> 4);
        target_rate = ((data[11] & 0x3e) >> 1);
        extension = ((data[10] & 0x1));
        ext_type = ((data[10] & 0xe) >> 1);
        sfreq = data[8] & 0xF;
        lfe = (data[13] & 0x18) >> 3;
        m_info.m_dataIsLE = true;
        bits = 14;
        break;

      // 16bit BE
      case DTS_PREAMBLE_16BE:
        dtsBlocks = (((data[4] & 0x1) << 7) | ((data[5] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[5] & 0x3) << 8) | data[6]) << 4) | ((data[7] & 0xF0) >> 4)) + 1;
        amode = ((data[7] & 0x0F) << 2) | ((data[8] & 0xC0) >> 6);
        sfreq = (data[8] & 0x3C) >> 2;
        target_rate = ((data[8] & 0x03) << 3) | ((data[9] & 0xe0) >> 5);
        extension = (data[10] & 0x10) >> 4;
        ext_type = (data[10] & 0xe0) >> 5;
        lfe = (data[10] >> 1) & 0x3;
        m_info.m_dataIsLE = false;
        bits = 16;
        break;

      // 16bit LE
      case DTS_PREAMBLE_16LE:
        dtsBlocks = (((data[5] & 0x1) << 7) | ((data[4] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[4] & 0x3) << 8) | data[7]) << 4) | ((data[6] & 0xF0) >> 4)) + 1;
        amode = ((data[6] & 0x0F) << 2) | ((data[9] & 0xC0) >> 6);
        sfreq = (data[9] & 0x3C) >> 2;
        target_rate = ((data[9] & 0x03) << 3) | ((data[8] & 0xe0) >> 5);
        extension = (data[11] & 0x10) >> 4;
        ext_type = (data[11] & 0xe0) >> 5;
        lfe = (data[11] >> 1) & 0x3;
        m_info.m_dataIsLE = true;
        bits = 16;
        break;

      default:
        continue;
    }

    if (sfreq == 0 || sfreq >= DTS_SFREQ_COUNT)
      continue;

    // AMODE is six bits, but only its first 16 values have a channel assignment;
    // the rest are user-defined and would index past DTSChannels
    if (amode >= sizeof(DTSChannels))
      continue;

    // make sure the framesize is sane
    if (m_fsize < 96 || m_fsize > 16384)
      continue;

    CAEStreamInfo::DataType dataType{CAEStreamInfo::STREAM_TYPE_NULL};
    switch (dtsBlocks << 5)
    {
      case 512:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_512;
        break;
      case 1024:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_1024;
        break;
      case 2048:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_2048;
        break;
    }

    if (dataType == CAEStreamInfo::STREAM_TYPE_NULL)
      continue;

    // adjust the fsize for 14 bit streams
    if (bits == 14)
      m_fsize = m_fsize / 14 * 16;

    // we need enough data to check for DTS-HD
    if (size - skip < m_fsize + 10)
    {
      // we can assume DTS sync at this point
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_needBytes = m_fsize + 10;
      m_fsize = 0;

      return skip;
    }

    // look for DTS-HD
    hd_sync = (data[m_fsize] << 24) | (data[m_fsize + 1] << 16) | (data[m_fsize + 2] << 8) |
              data[m_fsize + 3];
    if (hd_sync == DTS_PREAMBLE_HD)
    {
      int hd_size;
      bool blownup = (data[m_fsize + 5] & 0x20) != 0;
      if (blownup)
        hd_size = (((data[m_fsize + 6] & 0x01) << 19) | (data[m_fsize + 7] << 11) |
                   (data[m_fsize + 8] << 3) | ((data[m_fsize + 9] & 0xe0) >> 5)) +
                  1;
      else
        hd_size = (((data[m_fsize + 6] & 0x1f) << 11) | (data[m_fsize + 7] << 3) |
                   ((data[m_fsize + 8] & 0xe0) >> 5)) +
                  1;

      int header_size;
      if (blownup)
        header_size = (((data[m_fsize + 5] & 0x1f) << 7) | ((data[m_fsize + 6] & 0xfe) >> 1)) + 1;
      else
        header_size = (((data[m_fsize + 5] & 0x1f) << 3) | ((data[m_fsize + 6] & 0xe0) >> 5)) + 1;

      hd_sync = data[m_fsize + header_size] << 24 | data[m_fsize + header_size + 1] << 16 |
                data[m_fsize + header_size + 2] << 8 | data[m_fsize + header_size + 3];

      // set the type according to core or not
      if (m_coreOnly)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
      else if (hd_sync == DTS_PREAMBLE_XLL)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
      else if (hd_sync == DTS_PREAMBLE_XCH || hd_sync == DTS_PREAMBLE_XXCH ||
               hd_sync == DTS_PREAMBLE_X96K || hd_sync == DTS_PREAMBLE_XBR ||
               hd_sync == DTS_PREAMBLE_LBR)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD;
      else
        dataType = m_info.m_type;

      m_coreSize = m_fsize;
      m_fsize += hd_size;
    }

    unsigned int sampleRate = DTSSampleRates[sfreq];
    if (!m_hasSync || skip || dataType != m_info.m_type || sampleRate != m_info.m_sampleRate ||
        dtsBlocks != m_dtsBlocks)
    {
      m_hasSync = true;
      m_info.m_type = dataType;
      m_info.m_sampleRate = sampleRate;
      m_dtsBlocks = dtsBlocks;
      m_info.m_channels = DTSChannels[amode] + (lfe ? 1 : 0);
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_info.m_frameSize = m_fsize;
      m_info.m_repeat = 1;

      if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        m_info.m_channels += 2; // FIXME: this needs to be read out, not sure how to do that yet
        m_info.m_dtsPeriod = (192000 * (8 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD)
      {
        m_info.m_dtsPeriod = (192000 * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else
      {
        m_info.m_dtsPeriod =
            (m_info.m_sampleRate * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }

      std::string type;
      switch (dataType)
      {
        case CAEStreamInfo::STREAM_TYPE_DTSHD:
          type = "dtsHD";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
          type = "dtsHD MA";
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
          type = "dtsHD (core)";
          break;
        default:
          type = "dts";
          break;
      }

      if (extension)
      {
        switch (ext_type)
        {
          case 0:
            type += " XCH";
            break;
          case 2:
            type += " X96";
            break;
          case 6:
            type += " XXCH";
            break;
          default:
            type += " ext unknown";
            break;
        }
      }

      CLog::Log(LOGINFO,
                "CAEStreamParser::SyncDTS - {} stream detected ({} channels, {}Hz, {}bit {}, "
                "period: {}, syncword: 0x{:x}, target rate: 0x{:x}, framesize {}))",
                type, m_info.m_channels, m_info.m_sampleRate, bits, m_info.m_dataIsLE ? "LE" : "BE",
                m_info.m_dtsPeriod, hd_sync, target_rate, m_fsize);
    }

    return skip;
  }

  // lost sync
  CLog::Log(LOGINFO, "CAEStreamParser::SyncDTS - DTS sync lost");
  m_hasSync = false;
  return skip;
}

inline unsigned int CAEStreamParser::GetTrueHDChannels(const uint16_t chanmap)
{
  int channels = 0;
  for (int i = 0; i < 13; ++i)
    channels += THDChanMap[i] * ((chanmap >> i) & 1);
  return channels;
}

unsigned int CAEStreamParser::SyncTrueHD(uint8_t* data, unsigned int size)
{
  unsigned int left = size;
  unsigned int skip = 0;

  // if MLP
  for (; left; ++skip, ++data, --left)
  {
    // if we dont have sync and there is less the 8 bytes, then break out
    if (!m_hasSync && left < 8)
      return size;

    // if its a major audio unit
    uint16_t length = ((data[0] & 0x0F) << 8 | data[1]) << 1;
    uint32_t syncword = ((((data[4] << 8 | data[5]) << 8) | data[6]) << 8) | data[7];
    if (syncword == 0xf8726fba)
    {
      // we need 32 bytes to sync on a master audio unit
      if (left < 32)
        return skip;

      // get the rate and ensure its valid
      int rate = (data[8] & 0xf0) >> 4;
      if (rate == 0xF)
        continue;

      unsigned int major_sync_size = 28;
      if (data[29] & 1)
      {
        // extension(s) present, look up count
        int extension_count = data[30] >> 4;
        major_sync_size += 2 + extension_count * 2;
      }

      if (left < 4 + major_sync_size)
        return skip;

      // verify the crc of the audio unit
      uint16_t crc = av_crc(m_crcTrueHD, 0, data + 4, major_sync_size - 4);
      crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
      if (((data[4 + major_sync_size - 1] << 8) | data[4 + major_sync_size - 2]) != crc)
        continue;

      // TrueHD (0xba) only: MLP (0xbb) major sync has no channel_meaning()
      if (data[7] == 0xba)
        ProcessTrueHDDialNorm(data, major_sync_size);

      // get the sample rate and substreams, we have a valid master audio unit
      m_info.m_sampleRate = (rate & 0x8 ? 44100 : 48000) << (rate & 0x7);
      m_substreams = (data[20] & 0xF0) >> 4;

      // get the number of encoded channels
      uint16_t channel_map = ((data[10] & 0x1F) << 8) | data[11];
      if (!channel_map)
        channel_map = (data[9] << 1) | (data[10] >> 7);
      m_info.m_channels = CAEStreamParser::GetTrueHDChannels(channel_map);

      if (!m_hasSync)
        CLog::Log(LOGINFO,
                  "CAEStreamParser::SyncTrueHD - TrueHD stream detected ({} channels, {}Hz)",
                  m_info.m_channels, m_info.m_sampleRate);

      m_hasSync = true;
      m_fsize = length;
      m_info.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
      m_syncFunc = &CAEStreamParser::SyncTrueHD;
      m_info.m_frameSize = length;
      m_info.m_repeat = 1;
      return skip;
    }
    else
    {
      // we cant sink to a subframe until we have the information from a master audio unit
      if (!m_hasSync)
        continue;

      // if there is not enough data left to verify the packet, just return the skip amount
      if (left < (unsigned int)m_substreams * 4)
        return skip;

      // verify the parity
      int p = 0;
      uint8_t check = 0;
      for (int i = -1; i < m_substreams; ++i)
      {
        check ^= data[p++];
        check ^= data[p++];
        if (i == -1 || data[p - 2] & 0x80)
        {
          check ^= data[p++];
          check ^= data[p++];
        }
      }

      // if the parity nibble does not match
      if ((((check >> 4) ^ check) & 0xF) != 0xF)
      {
        // lost sync
        m_hasSync = false;
        CLog::Log(LOGINFO, "CAEStreamParser::SyncTrueHD - Sync Lost");
        continue;
      }
      else
      {
        m_fsize = length;
        return skip;
      }
    }
  }

  // lost sync
  m_hasSync = false;
  return skip;
}
