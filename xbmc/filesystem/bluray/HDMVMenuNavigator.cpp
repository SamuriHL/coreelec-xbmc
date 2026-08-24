/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HDMVMenuNavigator.h"

#include "BitReader.h"
#include "MPLSParser.h"
#include "PlaylistStructure.h"
#include "URL.h"
#include "filesystem/DiscDirectoryHelper.h"
#include "filesystem/File.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace XFILE
{
namespace
{
// index.bdmv / MovieObject.bdmv are tiny; the cap is only a corrupt-file guard
constexpr size_t MAX_NAVIGATION_FILE_SIZE{4 * 1024 * 1024};
// The menu's interactive composition sits at the start of its clip (epoch
// start). Menu clips can be hundreds of MB of looping video; the composition
// is within the first few. Bounded so a broken disc cannot make us read it all.
constexpr size_t MAX_M2TS_PROBE_SIZE{24 * 1024 * 1024};
constexpr unsigned int MAX_VM_STEPS{20000};
constexpr unsigned int MAX_CALL_DEPTH{16};
constexpr unsigned int MAX_MENUS_FOLLOWED{12};

constexpr uint32_t PSR_FLAG{0x80000000};

// HDMV navigation instruction - 12 bytes, layout per libbluray mobj_parse.c
// (mobj_parse_cmd): byte 0 = op_cnt(3) grp(2) sub_grp(3), byte 1 = imm_op1(1)
// imm_op2(1) reserved(2) branch_opt(4), byte 2 = reserved(4) cmp_opt(4),
// byte 3 = reserved(3) set_opt(5), then dst(32) src(32).
struct HdmvInsn
{
  uint8_t opCnt{0};
  uint8_t grp{0};
  uint8_t subGrp{0};
  uint8_t branchOpt{0};
  uint8_t cmpOpt{0};
  uint8_t setOpt{0};
  bool immOp1{false};
  bool immOp2{false};
  uint32_t dst{0};
  uint32_t src{0};
};

// instruction groups / options per libbluray hdmv_insn.h
enum InsnGroup : uint8_t
{
  GROUP_BRANCH = 0,
  GROUP_CMP = 1,
  GROUP_SET = 2,
};
enum BranchSubGroup : uint8_t
{
  BRANCH_GOTO = 0,
  BRANCH_JUMP = 1,
  BRANCH_PLAY = 2,
};
enum GotoOption : uint8_t
{
  INSN_NOP = 0,
  INSN_GOTO = 1,
  INSN_BREAK = 2,
};
enum JumpOption : uint8_t
{
  INSN_JUMP_OBJECT = 0,
  INSN_JUMP_TITLE = 1,
  INSN_CALL_OBJECT = 2,
  INSN_CALL_TITLE = 3,
  INSN_RESUME = 4,
};
enum PlayOption : uint8_t
{
  INSN_PLAY_PL = 0,
  INSN_PLAY_PL_PI = 1,
  INSN_PLAY_PL_PM = 2,
  INSN_TERMINATE_PL = 3,
  INSN_LINK_PI = 4,
  INSN_LINK_MK = 5,
};
enum CmpOption : uint8_t
{
  INSN_BC = 1,
  INSN_EQ = 2,
  INSN_NE = 3,
  INSN_GE = 4,
  INSN_GT = 5,
  INSN_LE = 6,
  INSN_LT = 7,
};
enum SetSubGroup : uint8_t
{
  SET_SET = 0,
  SET_SETSYSTEM = 1,
};
enum SetOption : uint8_t
{
  INSN_MOVE = 1,
  INSN_SWAP = 2,
  INSN_ADD = 3,
  INSN_SUB = 4,
  INSN_MUL = 5,
  INSN_DIV = 6,
  INSN_MOD = 7,
  INSN_RND = 8,
  INSN_AND = 9,
  INSN_OR = 10,
  INSN_XOR = 11,
  INSN_BITSET = 12,
  INSN_BITCLR = 13,
  INSN_SHL = 14,
  INSN_SHR = 15,
};
enum SetSystemOption : uint8_t
{
  INSN_SET_STREAM = 1,
  INSN_SET_BUTTON_PAGE = 3,
  INSN_SET_SEC_STREAM = 6,
};

HdmvInsn DecodeInsn(const std::span<std::byte> bytes, unsigned int offset)
{
  HdmvInsn insn;
  const uint8_t b0{GetByte(bytes, offset)};
  const uint8_t b1{GetByte(bytes, offset + 1)};
  const uint8_t b2{GetByte(bytes, offset + 2)};
  const uint8_t b3{GetByte(bytes, offset + 3)};
  insn.opCnt = b0 >> 5;
  insn.grp = (b0 >> 3) & 0x3;
  insn.subGrp = b0 & 0x7;
  insn.immOp1 = (b1 & 0x80) != 0;
  insn.immOp2 = (b1 & 0x40) != 0;
  insn.branchOpt = b1 & 0x0f;
  insn.cmpOpt = b2 & 0x0f;
  insn.setOpt = b3 & 0x1f;
  insn.dst = GetDWord(bytes, offset + 4);
  insn.src = GetDWord(bytes, offset + 8);
  return insn;
}

using HdmvObject = std::vector<HdmvInsn>;

struct HdmvIndex
{
  int firstPlayObject{-1}; // -1 = absent or BD-J
  int topMenuObject{-1};
  std::vector<int> titleObjects; // per title, -1 for BD-J titles
  bool valid{false};
};

std::vector<std::byte> ReadDiscFile(const std::string& discRoot,
                                    const std::string& relativePath,
                                    size_t maxSize)
{
  const std::string path{URIUtils::AddFileToFolder(discRoot, relativePath)};
  CFile file;
  if (!file.Open(path))
    return {};

  int64_t size{file.GetLength()};
  if (size <= 0 || std::cmp_greater(size, maxSize))
    size = static_cast<int64_t>(maxSize);

  std::vector<std::byte> buffer(static_cast<size_t>(size));
  const ssize_t read{file.Read(buffer.data(), size)};
  file.Close();
  if (read <= 0)
    return {};
  buffer.resize(static_cast<size_t>(read));
  return buffer;
}

// index.bdmv layout per libbluray index_parse.c: "INDX" + version, then
// indexes_start(32) at offset 8. At indexes_start: index_len(32), first_play
// and top_menu playback objects (4-byte type header + 8-byte object), then
// num_titles(16) and 12-byte title entries.
HdmvIndex ParseIndex(const std::vector<std::byte>& data)
{
  HdmvIndex index;
  std::span<std::byte> bytes{const_cast<std::byte*>(data.data()), data.size()};
  if (data.size() < 16 || GetString(bytes, 0, 4) != "INDX")
    return index;

  const uint32_t indexesStart{GetDWord(bytes, 8)};
  unsigned int offset{indexesStart};
  offset += 4; // index_len

  const auto parsePlaybackObject{[&bytes](unsigned int off) -> int
                                 {
                                   const uint8_t objectType{
                                       static_cast<uint8_t>(GetByte(bytes, off) >> 6)};
                                   if (objectType != 1) // 1 = HDMV, 2 = BD-J
                                     return -1;
                                   const uint16_t idRef{GetWord(bytes, off + 6)};
                                   return idRef == 0xffff ? -1 : idRef;
                                 }};

  index.firstPlayObject = parsePlaybackObject(offset);
  offset += 12;
  index.topMenuObject = parsePlaybackObject(offset);
  offset += 12;

  const uint16_t numTitles{GetWord(bytes, offset)};
  offset += 2;
  index.titleObjects.reserve(numTitles);
  for (unsigned int i = 0; i < numTitles; ++i)
  {
    index.titleObjects.emplace_back(parsePlaybackObject(offset));
    offset += 12;
  }
  index.valid = true;
  return index;
}

// MovieObject.bdmv layout per libbluray mobj_parse.c: "MOBJ" + version, data
// at byte 40: data_len(32) reserved(32) num_objects(16), then per object
// flags(16) num_cmds(16) and 12-byte commands.
std::vector<HdmvObject> ParseMovieObjects(const std::vector<std::byte>& data)
{
  std::vector<HdmvObject> objects;
  std::span<std::byte> bytes{const_cast<std::byte*>(data.data()), data.size()};
  if (data.size() < 50 || GetString(bytes, 0, 4) != "MOBJ")
    return objects;

  unsigned int offset{40};
  offset += 4; // data_len
  offset += 4; // reserved
  const uint16_t numObjects{GetWord(bytes, offset)};
  offset += 2;

  objects.reserve(numObjects);
  for (unsigned int i = 0; i < numObjects; ++i)
  {
    offset += 2; // resume_intention / menu_call_mask / title_search_mask + padding
    const uint16_t numCmds{GetWord(bytes, offset)};
    offset += 2;
    HdmvObject object;
    object.reserve(numCmds);
    for (unsigned int c = 0; c < numCmds; ++c)
    {
      object.emplace_back(DecodeInsn(bytes, offset));
      offset += 12;
    }
    objects.emplace_back(std::move(object));
  }
  return objects;
}

// PSR boot values per libbluray register.c bd_psr_init - discs branch on
// these (audio caps, region, profile) before reaching their episode dispatch
constexpr std::array<uint32_t, 128> MakePsrDefaults()
{
  std::array<uint32_t, 128> psr{};
  psr[0] = 1; // IG stream number
  psr[1] = 0xff; // primary audio
  psr[2] = 0x0fff0fff; // PG/TextST + PiP PG
  psr[3] = 1; // angle
  psr[4] = 0xffff; // title
  psr[5] = 0xffff; // chapter
  psr[10] = 0xffff; // selected button
  psr[12] = 0xff; // user style
  psr[13] = 0xff; // parental / user age
  psr[14] = 0xffff; // secondary audio/video
  psr[15] = 0x1ffff; // audio capability (all common codecs)
  psr[16] = 0xffffff; // audio language
  psr[17] = 0xffffff; // PG language
  psr[18] = 0xffffff; // menu language
  psr[19] = 0xffff; // country
  psr[20] = 1; // region A
  psr[21] = 0; // prefer 2D
  psr[29] = 0x3; // video capability
  psr[30] = 0x1ffff; // TextST capability
  psr[31] = 0x00000200 | (2u << 16); // profile 2 v2.0
  psr[36] = 0xffff; // backup PSR4
  return psr;
}
constexpr std::array<uint32_t, 128> PSR_DEFAULTS{MakePsrDefaults()};

struct VmState
{
  std::array<uint32_t, 4096> gpr{};
};

struct VmRun
{
  std::vector<unsigned int> played; // playlists, in order
  int pageTurn{-1}; // SET_BUTTON_PAGE page id, buttons only
  bool parkedOnMenu{false};
};

/*!
 * Executes HDMV navigation programs (Movie Objects and IG button command
 * lists) far enough to learn which playlists they start. Semantics per
 * libbluray hdmv_vm.c: register/immediate operand fetch with the
 * SET_STREAM / SET_BUTTON_PAGE special register reads, false compares skip
 * exactly one instruction, GOTO/BREAK, JUMP/CALL through the title index,
 * saturating SUB and guarded DIV/MOD. Deliberately unmodelled (recorded or
 * ignored, never executed): stream selection, stills, popup state, timers -
 * playback side effects a static scan neither has nor needs.
 */
class CHdmvVm
{
public:
  CHdmvVm(const std::vector<HdmvObject>& objects,
          const HdmvIndex& index,
          const std::set<unsigned int>& menuPlaylists)
    : m_objects(objects), m_index(index), m_menuPlaylists(menuPlaylists)
  {
  }

  //! Run a movie object. Stops at the first menu-carrying playlist played
  //! (the state then is the state in force while that menu shows - pressing
  //! its buttons must start from here, not from the program's end).
  void RunObject(int objectId, VmState& state, VmRun& run) const
  {
    if (objectId < 0 || std::cmp_greater_equal(objectId, m_objects.size()))
      return;
    Execute(m_objects[static_cast<size_t>(objectId)], objectId, state, run, false);
  }

  //! Run a button's command list (button semantics: SET_BUTTON_PAGE ends the
  //! list; jumps continue into movie objects to find what finally plays).
  void RunButton(const HdmvObject& commands, VmState& state, VmRun& run) const
  {
    Execute(commands, BUTTON_OBJECT_ID, state, run, true);
  }

private:
  static constexpr int BUTTON_OBJECT_ID{0xffff};

  uint32_t ReadReg(const VmState& state, uint32_t reg) const
  {
    if (reg & PSR_FLAG)
    {
      if (reg & ~(PSR_FLAG | 0x7f))
        return 0;
      return PSR_DEFAULTS[reg & 0x7f];
    }
    if (reg & ~0xfffu)
      return 0;
    return state.gpr[reg];
  }

  // SET_STREAM/SET_SEC_STREAM register operand: two 12-bit GPR ids whose
  // values fill the stream-number fields, flag bits preserved (hdmv_vm.c
  // _read_setstream_regs)
  uint32_t ReadSetStreamRegs(const VmState& state, uint32_t value) const
  {
    const uint32_t flags{value & 0xf000f000};
    const uint32_t val0{state.gpr[value & 0xfff] & 0x0fff};
    const uint32_t val1{state.gpr[(value >> 16) & 0xfff] & 0x0fff};
    return flags | val0 | (val1 << 16);
  }

  // SET_BUTTON_PAGE register operand: 12-bit GPR id, top two flag bits
  // preserved (hdmv_vm.c _read_setbuttonpage_reg)
  uint32_t ReadSetButtonPageReg(const VmState& state, uint32_t value) const
  {
    const uint32_t flags{value & 0xc0000000};
    const uint32_t val0{state.gpr[value & 0xfff] & 0x3fffffff};
    return flags | val0;
  }

  static void StoreReg(VmState& state, uint32_t reg, uint32_t value)
  {
    // stores to PSRs or through immediates are invalid; ignore like libbluray
    if ((reg & PSR_FLAG) || (reg & ~0xfffu))
      return;
    state.gpr[reg] = value;
  }

  int ResolveTitleObject(uint32_t title) const
  {
    if (title == 0) // JUMP_TITLE 0 = top menu
      return m_index.topMenuObject;
    if (std::cmp_less_equal(title, m_index.titleObjects.size()))
      return m_index.titleObjects[title - 1];
    return -1;
  }

  void Execute(
      const HdmvObject& entry, int entryId, VmState& state, VmRun& run, bool buttonContext) const
  {
    const HdmvObject* list{&entry};
    int listId{entryId};
    uint32_t pc{0};
    unsigned int steps{0};
    // per-instruction set of register-state hashes seen there ("going round"
    // = returning to an instruction in a state already seen at it)
    std::map<uint64_t, std::set<uint64_t>> visited;
    std::vector<std::pair<int, uint32_t>> callStack;

    while (true)
    {
      if (pc >= list->size())
      {
        if (callStack.empty())
          return;
        // end of called object: resume the caller (RESUME semantics kept
        // simple - PSR backup/restore has no observable effect here)
        listId = callStack.back().first;
        pc = callStack.back().second;
        callStack.pop_back();
        list = listId == BUTTON_OBJECT_ID ? &entry
                                          : &m_objects[static_cast<size_t>(listId)];
        continue;
      }
      if (++steps > MAX_VM_STEPS)
        return;
      // Loop guard, state-sensitive: a program may legitimately loop while
      // WRITING - a play-sequence walker (play what a GPR names, advance it,
      // go back) revisits its instructions with changing registers and must
      // keep running. Stopping on the bare revisit killed such a program one
      // instruction short of its JUMP_TITLE 0 (TNG S7 language button -
      // found on the donor side, same defect here). "Going round" means
      // returning to an instruction in a state already seen there; a menu
      // idling on screen changes nothing and still stops.
      uint64_t stateHash{1469598103934665603ull};
      for (const uint32_t reg : state.gpr)
      {
        stateHash ^= reg;
        stateHash *= 1099511628211ull;
      }
      if (!visited[(static_cast<uint64_t>(listId) << 20) | pc].insert(stateHash).second)
        return;

      const HdmvInsn& insn{(*list)[pc]};

      // operand fetch (hdmv_vm.c _fetch_operands)
      const bool setStream{insn.grp == GROUP_SET && insn.subGrp == SET_SETSYSTEM &&
                           (insn.setOpt == INSN_SET_STREAM || insn.setOpt == INSN_SET_SEC_STREAM)};
      const bool setButtonPage{insn.grp == GROUP_SET && insn.subGrp == SET_SETSYSTEM &&
                               insn.setOpt == INSN_SET_BUTTON_PAGE};
      uint32_t dst{0};
      uint32_t src{0};
      if (insn.opCnt > 0)
        dst = insn.immOp1 ? insn.dst
              : setStream ? ReadSetStreamRegs(state, insn.dst)
              : setButtonPage ? ReadSetButtonPageReg(state, insn.dst)
                              : ReadReg(state, insn.dst);
      if (insn.opCnt > 1)
        src = insn.immOp2 ? insn.src
              : setStream ? ReadSetStreamRegs(state, insn.src)
              : setButtonPage ? ReadSetButtonPageReg(state, insn.src)
                              : ReadReg(state, insn.src);

      uint32_t nextPc{pc + 1};

      switch (insn.grp)
      {
        case GROUP_BRANCH:
          switch (insn.subGrp)
          {
            case BRANCH_GOTO:
              if (insn.branchOpt == INSN_GOTO)
                nextPc = dst;
              else if (insn.branchOpt == INSN_BREAK)
                return;
              break;

            case BRANCH_JUMP:
            {
              int target{-1};
              bool call{false};
              switch (insn.branchOpt)
              {
                case INSN_JUMP_OBJECT:
                  target = static_cast<int>(dst);
                  break;
                case INSN_JUMP_TITLE:
                  target = ResolveTitleObject(dst);
                  break;
                case INSN_CALL_OBJECT:
                  target = static_cast<int>(dst);
                  call = true;
                  break;
                case INSN_CALL_TITLE:
                  target = ResolveTitleObject(dst);
                  call = true;
                  break;
                case INSN_RESUME:
                  if (callStack.empty())
                    return;
                  listId = callStack.back().first;
                  nextPc = callStack.back().second;
                  callStack.pop_back();
                  list = listId == BUTTON_OBJECT_ID
                             ? &entry
                             : &m_objects[static_cast<size_t>(listId)];
                  pc = nextPc;
                  continue;
                default:
                  break;
              }
              if (target < 0 || std::cmp_greater_equal(target, m_objects.size()))
                return; // BD-J title or invalid object: nothing further to learn
              if (call)
              {
                if (callStack.size() >= MAX_CALL_DEPTH)
                  return;
                callStack.emplace_back(listId, pc + 1);
              }
              listId = target;
              list = &m_objects[static_cast<size_t>(target)];
              pc = 0;
              continue;
            }

            case BRANCH_PLAY:
              if (insn.branchOpt == INSN_PLAY_PL || insn.branchOpt == INSN_PLAY_PL_PI ||
                  insn.branchOpt == INSN_PLAY_PL_PM)
              {
                run.played.emplace_back(dst);
                // a menu-carrying playlist is where the disc parks and shows
                // its menu: the register state NOW is what its buttons see
                if (m_menuPlaylists.contains(dst))
                {
                  run.parkedOnMenu = true;
                  return;
                }
              }
              // TERMINATE_PL / LINK_PI / LINK_MK: keep walking the program
              break;

            default:
              break;
          }
          break;

        case GROUP_CMP:
        {
          bool result{true};
          switch (insn.cmpOpt)
          {
            case INSN_BC:
              // true when dst is contained in the src mask (hdmv_vm.c skips
              // on !!(dst & ~src))
              result = (dst & ~src) == 0;
              break;
            case INSN_EQ:
              result = dst == src;
              break;
            case INSN_NE:
              result = dst != src;
              break;
            case INSN_GE:
              result = dst >= src;
              break;
            case INSN_GT:
              result = dst > src;
              break;
            case INSN_LE:
              result = dst <= src;
              break;
            case INSN_LT:
              result = dst < src;
              break;
            default:
              break;
          }
          if (!result)
            ++nextPc; // false compare skips exactly one instruction
          break;
        }

        case GROUP_SET:
          if (insn.subGrp == SET_SET)
          {
            const uint32_t dst0{dst};
            switch (insn.setOpt)
            {
              case INSN_MOVE:
                dst = src;
                break;
              case INSN_SWAP:
                if (!insn.immOp1 && !insn.immOp2)
                {
                  StoreReg(state, insn.src, dst);
                  StoreReg(state, insn.dst, src);
                }
                dst = dst0; // handled above; suppress the generic store
                break;
              case INSN_ADD:
                dst = dst + src;
                break;
              case INSN_SUB:
                dst = dst > src ? dst - src : 0;
                break;
              case INSN_MUL:
                dst = dst * src;
                break;
              case INSN_DIV:
                dst = src > 0 ? dst / src : 0xffffffff;
                break;
              case INSN_MOD:
                dst = src > 0 ? dst % src : 0xffffffff;
                break;
              case INSN_RND:
                // deterministic: a scan must be reproducible; 1 is in-range
                dst = src > 0 ? 1 : 0;
                break;
              case INSN_AND:
                dst &= src;
                break;
              case INSN_OR:
                dst |= src;
                break;
              case INSN_XOR:
                dst ^= src;
                break;
              case INSN_BITSET:
                if (src < 32)
                  dst |= (1u << src);
                break;
              case INSN_BITCLR:
                if (src < 32)
                  dst &= ~(1u << src);
                break;
              case INSN_SHL:
                if (src < 32)
                  dst <<= src;
                break;
              case INSN_SHR:
                if (src < 32)
                  dst >>= src;
                break;
              default:
                break;
            }
            if (dst != dst0 && !insn.immOp1)
              StoreReg(state, insn.dst, dst);
          }
          else if (insn.subGrp == SET_SETSYSTEM)
          {
            if (insn.setOpt == INSN_SET_BUTTON_PAGE && buttonContext)
            {
              // operand 2 = page: bit 31 = page named, low 8 bits = page id.
              // SET_BUTTON_PAGE ends a BUTTON's command list (spec; libbluray
              // ButtonPage handling) - but not a movie object's program.
              if (src & 0x80000000)
                run.pageTurn = static_cast<int>(src & 0xff);
              return;
            }
            // SET_STREAM, ENABLE/DISABLE_BUTTON, POPUP_OFF, STILL_*,
            // SET_NV_TIMER: no playback to affect - continue
          }
          break;

        default:
          break;
      }

      pc = nextPc;
    }
  }

  const std::vector<HdmvObject>& m_objects;
  const HdmvIndex& m_index;
  const std::set<unsigned int>& m_menuPlaylists;
};

// ---------------------------------------------------------------------------
// IG (Interactive Graphics) structural parsing - pages / BOGs / buttons and
// their commands only. No object (bitmap) or palette decoding: playlist
// identity needs the buttons' commands, not their artwork.

struct IgButton
{
  uint16_t id{0};
  uint16_t numericSelect{0xffff};
  bool autoAction{false};
  uint16_t x{0};
  uint16_t y{0};
  uint16_t lower{0xffff};
  HdmvObject commands;
};

struct IgBog
{
  uint16_t defaultValidButton{0xffff};
  std::vector<IgButton> buttons;
};

struct IgPage
{
  uint8_t id{0};
  uint16_t defaultSelectedButton{0xffff};
  std::vector<IgBog> bogs;
};

struct IgComposition
{
  bool popupUi{false};
  std::vector<IgPage> pages;
  bool valid{false};
};

// Extract the elementary-stream bytes of one PID from a BDAV (192-byte
// packet) transport stream, PES headers stripped.
std::vector<std::byte> ExtractElementaryStream(const std::vector<std::byte>& m2ts, unsigned int pid)
{
  std::vector<std::byte> es;
  const std::span<std::byte> bytes{const_cast<std::byte*>(m2ts.data()), m2ts.size()};
  size_t offset{0};

  while (offset + 192 <= m2ts.size())
  {
    if (GetByte(bytes, static_cast<unsigned int>(offset) + 4) != 0x47)
    {
      ++offset; // resync byte-wise on damage
      continue;
    }
    const unsigned int tsOff{static_cast<unsigned int>(offset) + 4};
    const uint8_t b1{GetByte(bytes, tsOff + 1)};
    const uint8_t b2{GetByte(bytes, tsOff + 2)};
    const unsigned int packetPid{static_cast<unsigned int>((b1 & 0x1f) << 8 | b2)};
    if (packetPid == pid)
    {
      const bool payloadStart{(b1 & 0x40) != 0};
      const uint8_t adaptation{static_cast<uint8_t>((GetByte(bytes, tsOff + 3) >> 4) & 0x3)};
      unsigned int payloadOff{tsOff + 4};
      if (adaptation & 0x2) // adaptation field present
        payloadOff += 1u + GetByte(bytes, payloadOff);
      if (adaptation & 0x1) // payload present
      {
        unsigned int end{tsOff + 188};
        if (payloadOff < end)
        {
          if (payloadStart)
          {
            // strip the PES header: 00 00 01 sid len(2) flags(2) hdrlen(1)
            if (end - payloadOff >= 9 && GetByte(bytes, payloadOff) == 0 &&
                GetByte(bytes, payloadOff + 1) == 0 && GetByte(bytes, payloadOff + 2) == 1)
              payloadOff += 9u + GetByte(bytes, payloadOff + 8);
            else
              payloadOff = end; // not a PES start: skip
          }
          for (unsigned int i = payloadOff; i < end; ++i)
            es.emplace_back(bytes[i]);
        }
      }
    }
    offset += 192;
  }
  return es;
}

// Skip an effect sequence (windows + effects with composition objects) -
// layout per libbluray ig_decode.c/pg_decode.c. Returns the new offset.
unsigned int SkipEffectSequence(const std::span<std::byte> bytes, unsigned int offset)
{
  const uint8_t numWindows{GetByte(bytes, offset)};
  offset += 1 + numWindows * 9u; // id(1) x(2) y(2) w(2) h(2)
  const uint8_t numEffects{GetByte(bytes, offset)};
  offset += 1;
  for (unsigned int e = 0; e < numEffects; ++e)
  {
    offset += 4; // duration(3) palette(1)
    const uint8_t numObjects{GetByte(bytes, offset)};
    offset += 1;
    for (unsigned int o = 0; o < numObjects; ++o)
    {
      const bool cropped{(GetByte(bytes, offset + 3) & 0x80) != 0};
      offset += 8 + (cropped ? 8u : 0u); // objid(2) win(1) flags(1) x(2) y(2) [crop(8)]
    }
  }
  return offset;
}

// Parse an assembled interactive_composition() - layout per libbluray
// ig_decode.c _decode_interactive_composition.
IgComposition ParseInteractiveComposition(const std::vector<std::byte>& data)
{
  IgComposition composition;
  const std::span<std::byte> bytes{const_cast<std::byte*>(data.data()), data.size()};
  unsigned int offset{3}; // data_len(24) - fragments already assembled

  const uint8_t models{GetByte(bytes, offset)};
  const bool streamModel{(models & 0x80) != 0}; // 0 = in-mux
  composition.popupUi = (models & 0x40) != 0;
  offset += 1;
  if (!streamModel)
    offset += 10; // composition/selection timeout PTS (2 x 40 bits)
  offset += 3; // user_timeout_duration

  const uint8_t numPages{GetByte(bytes, offset)};
  offset += 1;
  composition.pages.reserve(numPages);
  for (unsigned int p = 0; p < numPages; ++p)
  {
    IgPage page;
    page.id = GetByte(bytes, offset);
    offset += 2; // id(1) version(1)
    offset += 8; // UO mask table
    offset = SkipEffectSequence(bytes, offset); // in effects
    offset = SkipEffectSequence(bytes, offset); // out effects
    offset += 1; // animation frame rate
    page.defaultSelectedButton = GetWord(bytes, offset);
    offset += 4; // default selected(2) + default activated(2)
    offset += 1; // palette id
    const uint8_t numBogs{GetByte(bytes, offset)};
    offset += 1;
    page.bogs.reserve(numBogs);
    for (unsigned int b = 0; b < numBogs; ++b)
    {
      IgBog bog;
      bog.defaultValidButton = GetWord(bytes, offset);
      offset += 2;
      const uint8_t numButtons{GetByte(bytes, offset)};
      offset += 1;
      bog.buttons.reserve(numButtons);
      for (unsigned int n = 0; n < numButtons; ++n)
      {
        IgButton button;
        button.id = GetWord(bytes, offset);
        button.numericSelect = GetWord(bytes, offset + 2);
        button.autoAction = (GetByte(bytes, offset + 4) & 0x80) != 0;
        button.x = GetWord(bytes, offset + 5);
        button.y = GetWord(bytes, offset + 7);
        button.lower = GetWord(bytes, offset + 11); // upper(2) LOWER(2) left(2) right(2)
        offset += 33; // fixed part incl. state object refs and sound ids
        const uint16_t numCommands{GetWord(bytes, offset)};
        offset += 2;
        button.commands.reserve(numCommands);
        for (unsigned int c = 0; c < numCommands; ++c)
        {
          button.commands.emplace_back(DecodeInsn(bytes, offset));
          offset += 12;
        }
        bog.buttons.emplace_back(std::move(button));
      }
      page.bogs.emplace_back(std::move(bog));
    }
    composition.pages.emplace_back(std::move(page));
  }
  composition.valid = !composition.pages.empty();
  return composition;
}

// Walk PGS-framed segments (type(1) length(2)) and assemble the first
// complete interactive composition from its ICS fragments. ICS payload:
// video_descriptor(5) composition_descriptor(3) sequence_descriptor(1)
// [first=0x80 last=0x40], fragment data follows (libbluray
// graphics_processor.c segment offsets).
IgComposition ParseFirstComposition(const std::vector<std::byte>& es)
{
  constexpr uint8_t PGS_IG_COMPOSITION{0x18};
  constexpr std::array<uint8_t, 6> KNOWN_SEGMENTS{0x14, 0x15, 0x16, 0x17, 0x18, 0x80};

  const std::span<std::byte> bytes{const_cast<std::byte*>(es.data()), es.size()};
  std::vector<std::byte> assembled;
  bool assembling{false};
  unsigned int offset{0};

  while (static_cast<size_t>(offset) + 3 <= es.size())
  {
    const uint8_t type{GetByte(bytes, offset)};
    const uint16_t length{GetWord(bytes, offset + 1)};
    if (std::ranges::find(KNOWN_SEGMENTS, type) == KNOWN_SEGMENTS.end())
      break; // lost sync - work with what is assembled so far
    if (static_cast<size_t>(offset) + 3 + length > es.size())
      break;

    if (type == PGS_IG_COMPOSITION && length > 9)
    {
      const unsigned int payload{offset + 3};
      const uint8_t sequenceFlags{GetByte(bytes, payload + 8)};
      const bool first{(sequenceFlags & 0x80) != 0};
      const bool last{(sequenceFlags & 0x40) != 0};
      if (first)
      {
        assembled.clear();
        assembling = true;
      }
      if (assembling)
      {
        for (unsigned int i = payload + 9; i < payload + length; ++i)
          assembled.emplace_back(bytes[i]);
        if (last)
          return ParseInteractiveComposition(assembled);
      }
    }
    offset += 3u + length;
  }
  return {};
}

// Where a playlist's IG stream lives: the clip file to read and the PID to
// read from it. Out-of-mux IG (STN entry referencing a sub-path) reads the
// SUB-PATH's clip - TNG carries its menus that way; the play-item clips
// alone would find no menu on the whole disc.
struct IgLocation
{
  unsigned int clip{0};
  unsigned int pid{0};
  bool found{false};
};

IgLocation FindIgLocation(const CURL& url, unsigned int playlist)
{
  IgLocation location;
  BlurayPlaylistInformation info;
  std::map<unsigned int, ClipInformation> clipCache;
  if (!CMPLSParser::ReadMPLS(url, playlist, info, clipCache, StreamDetails::DEFER))
    return location;

  for (const PlayItemInformation& playItem : info.playItems)
  {
    for (const StreamInformation& stream : playItem.interactiveGraphicStreams)
    {
      if (stream.type == BLURAY_STREAM_TYPE::SUBPATH)
      {
        if (stream.subpathId < info.subPlayItems.size() &&
            stream.subclipId < info.subPlayItems[stream.subpathId].clips.size())
        {
          location.clip = info.subPlayItems[stream.subpathId].clips[stream.subclipId].clip;
          location.pid = stream.packetIdentifier;
          location.found = true;
          return location;
        }
      }
      else if (!playItem.angleClips.empty())
      {
        location.clip = playItem.angleClips.front().clip;
        location.pid = stream.packetIdentifier;
        location.found = true;
        return location;
      }
    }
  }
  return location;
}

// Present a page's buttons in the order the disc states: the numeric-select
// values when they form a gapless run (the disc numbering its own entries
// for the remote), otherwise chains along the stated lower-neighbour links,
// otherwise reading order. Button ids are NOT presentation order.
std::vector<const IgButton*> OrderButtons(const IgPage& page)
{
  std::vector<const IgButton*> buttons;
  for (const IgBog& bog : page.bogs)
    for (const IgButton& button : bog.buttons)
      if (!button.commands.empty())
        buttons.emplace_back(&button);

  // numeric select: require a gapless 1..N (or k..k+N-1) run to trust it
  std::vector<const IgButton*> numbered{buttons};
  std::erase_if(numbered, [](const IgButton* b) { return b->numericSelect == 0xffff; });
  if (numbered.size() == buttons.size() && buttons.size() > 1)
  {
    std::ranges::sort(numbered, {}, [](const IgButton* b) { return b->numericSelect; });
    const bool gapless{[&numbered]
                       {
                         for (size_t i = 1; i < numbered.size(); ++i)
                           if (numbered[i]->numericSelect != numbered[i - 1]->numericSelect + 1)
                             return false;
                         return true;
                       }()};
    if (gapless)
      return numbered;
  }

  // neighbour chains: walk the stated lower links from each chain head
  std::set<uint16_t> referenced;
  std::map<uint16_t, const IgButton*> byId;
  for (const IgButton* button : buttons)
    byId.try_emplace(button->id, button);
  for (const IgButton* button : buttons)
    if (button->lower != button->id && byId.contains(button->lower))
      referenced.insert(button->lower);

  std::vector<const IgButton*> heads;
  for (const IgButton* button : buttons)
    if (!referenced.contains(button->id))
      heads.emplace_back(button);

  if (!heads.empty() && heads.size() < buttons.size())
  {
    std::ranges::sort(heads, [](const IgButton* a, const IgButton* b)
                      { return std::tie(a->x, a->y) < std::tie(b->x, b->y); });
    std::vector<const IgButton*> ordered;
    std::set<uint16_t> seen;
    for (const IgButton* head : heads)
    {
      const IgButton* current{head};
      while (current && seen.insert(current->id).second)
      {
        ordered.emplace_back(current);
        const auto it{byId.find(current->lower)};
        current = (it != byId.end() && it->second != current) ? it->second : nullptr;
      }
    }
    if (ordered.size() == buttons.size())
      return ordered;
  }

  std::ranges::sort(buttons, [](const IgButton* a, const IgButton* b)
                    { return std::tie(a->y, a->x) < std::tie(b->y, b->x); });
  return buttons;
}
} // unnamed namespace

CHDMVMenuNavigator::MenuStatedEpisodes CHDMVMenuNavigator::GetMenuStatedEpisodes(
    const CURL& url,
    const std::map<unsigned int, PlaylistInformation>& playlists,
    std::chrono::milliseconds minEpisodeDuration)
{
  MenuStatedEpisodes result;
  try
  {
    const std::string& discRoot{url.GetHostName()};

    const std::vector<std::byte> indexData{ReadDiscFile(
        discRoot, URIUtils::AddFileToFolder("BDMV", "index.bdmv"), MAX_NAVIGATION_FILE_SIZE)};
    const HdmvIndex index{ParseIndex(indexData)};
    if (!index.valid || (index.firstPlayObject < 0 && index.topMenuObject < 0))
    {
      CLog::LogF(LOGDEBUG, "no HDMV first-play/top-menu (BD-J disc?) - menu not consulted");
      return result;
    }

    const std::vector<std::byte> mobjData{ReadDiscFile(
        discRoot, URIUtils::AddFileToFolder("BDMV", "MovieObject.bdmv"), MAX_NAVIGATION_FILE_SIZE)};
    const std::vector<HdmvObject> objects{ParseMovieObjects(mobjData)};
    if (objects.empty())
      return result;

    // which playlists carry a menu composition (STN-declared IG, in-mux or
    // via sub-path) - also where to read each menu's IG from
    std::set<unsigned int> menuPlaylists;
    std::map<unsigned int, IgLocation> igLocations;
    for (const auto& entry : playlists)
    {
      const unsigned int playlist{entry.first};
      const IgLocation location{FindIgLocation(url, playlist)};
      if (location.found)
      {
        menuPlaylists.insert(playlist);
        igLocations.try_emplace(playlist, location);
      }
    }
    if (menuPlaylists.empty())
      return result;

    const CHdmvVm vm(objects, index, menuPlaylists);

    // run the disc's own boot path to the menu it parks on
    VmState state;
    VmRun boot;
    vm.RunObject(index.firstPlayObject, state, boot);
    if (!boot.parkedOnMenu && index.topMenuObject >= 0)
    {
      state = VmState{};
      boot = VmRun{};
      vm.RunObject(index.topMenuObject, state, boot);
    }
    if (!boot.parkedOnMenu)
    {
      CLog::LogF(LOGDEBUG, "disc navigation never reached a menu-carrying playlist");
      return result;
    }
    const unsigned int firstMenu{boot.played.back()};

    const auto isEpisodeLength{[&playlists, minEpisodeDuration](unsigned int playlist)
                               {
                                 const auto it{playlists.find(playlist)};
                                 return it != playlists.end() &&
                                        it->second.duration >= minEpisodeDuration;
                               }};

    // breadth-first over the menus the buttons lead to
    std::deque<unsigned int> menuQueue{firstMenu};
    std::set<unsigned int> menusVisited{firstMenu};
    std::vector<unsigned int> episodePlaylists;
    std::set<unsigned int> episodeSeen;
    std::set<unsigned int> playAllSeen;

    while (!menuQueue.empty() && menusVisited.size() <= MAX_MENUS_FOLLOWED)
    {
      const unsigned int menuPlaylist{menuQueue.front()};
      menuQueue.pop_front();

      const IgLocation& location{igLocations.at(menuPlaylist)};
      const std::vector<std::byte> m2ts{
          ReadDiscFile(discRoot,
                       URIUtils::AddFileToFolder("BDMV", "STREAM",
                                                 fmt::format("{:05}.m2ts", location.clip)),
                       MAX_M2TS_PROBE_SIZE)};
      if (m2ts.empty())
        continue;
      const IgComposition composition{
          ParseFirstComposition(ExtractElementaryStream(m2ts, location.pid))};
      if (!composition.valid)
      {
        CLog::LogF(LOGDEBUG, "playlist {} clip {} PID {:#x}: no interactive composition found",
                   menuPlaylist, location.clip, location.pid);
        continue;
      }
      CLog::LogF(LOGDEBUG, "playlist {} menu: {} page(s), ui model {}", menuPlaylist,
                 composition.pages.size(), composition.popupUi ? "pop-up" : "always-on");

      for (const IgPage& page : composition.pages)
      {
        for (const IgButton* button : OrderButtons(page))
        {
          VmState buttonState{state};
          VmRun run;
          vm.RunButton(button->commands, buttonState, run);

          std::vector<unsigned int> episodeTargets;
          for (const unsigned int played : run.played)
          {
            if (isEpisodeLength(played) &&
                std::ranges::find(episodeTargets, played) == episodeTargets.end())
              episodeTargets.emplace_back(played);
            // buttons can also lead to further menus (page structure aside)
            if (menuPlaylists.contains(played) && !menusVisited.contains(played))
            {
              menusVisited.insert(played);
              menuQueue.emplace_back(played);
            }
          }

          if (episodeTargets.size() == 1)
          {
            if (episodeSeen.insert(episodeTargets.front()).second)
              episodePlaylists.emplace_back(episodeTargets.front());
          }
          else if (episodeTargets.size() >= 2)
          {
            // PLAY ALL: names no single episode, but states the full set
            for (const unsigned int target : episodeTargets)
              playAllSeen.insert(target);
          }
        }
      }
    }

    if (episodePlaylists.size() >= 2)
    {
      result.episodePlaylists = std::move(episodePlaylists);
      result.playAllPlaylists.assign(playAllSeen.begin(), playAllSeen.end());
      result.valid = true;
      CLog::LogF(LOGINFO, "disc menu states {} episode playlist(s): [{}]",
                 result.episodePlaylists.size(),
                 fmt::join(result.episodePlaylists, ", "));
    }
    else
    {
      CLog::LogF(LOGDEBUG, "menu stated {} episode playlist(s) - not enough to trust",
                 episodePlaylists.size());
    }
  }
  catch (const std::exception& e)
  {
    CLog::LogF(LOGWARNING, "HDMV menu scan failed - {} (falling back to duration heuristics)",
               e.what());
    return {};
  }
  return result;
}
} // namespace XFILE
