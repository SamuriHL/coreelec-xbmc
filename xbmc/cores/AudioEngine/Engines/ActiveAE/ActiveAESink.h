/*
 *  Copyright (C) 2010-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAEBuffer.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Interfaces/AESink.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/SystemClock.h"
#include "threads/Thread.h"
#include "utils/ActorProtocol.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

class CAEBitstreamPacker;

namespace ActiveAE
{
using namespace Actor;

class CEngineStats;

struct SinkConfig
{
  AEAudioFormat format;
  CEngineStats *stats;
  const std::string *device;
};

struct SinkReply
{
  AEAudioFormat format;
  const std::string* device;
  float cacheTotal;
  float latency;
  bool hasVolume;
};

class CSinkControlProtocol : public Protocol
{
public:
  CSinkControlProtocol(std::string name, CEvent* inEvent, CEvent* outEvent)
    : Protocol(std::move(name), inEvent, outEvent)
  {
  }
  enum OutSignal
  {
    CONFIGURE,
    UNCONFIGURE,
    STREAMING,
    APPFOCUSED,
    RESERVE,
    VOLUME,
    FLUSH,
    TIMEOUT,
    SETSILENCETIMEOUT,
    SETNOISETYPE,
    HOLDBURSTS,
  };
  enum InSignal
  {
    ACC,
    ERR,
    STATS,
  };
};

class CSinkDataProtocol : public Protocol
{
public:
  CSinkDataProtocol(std::string name, CEvent* inEvent, CEvent* outEvent)
    : Protocol(std::move(name), inEvent, outEvent)
  {
  }
  enum OutSignal
  {
    SAMPLE = 0,
    DRAIN,
  };
  enum InSignal
  {
    RETURNSAMPLE,
    ACC,
  };
};

class CActiveAESink : private CThread
{
public:
  explicit CActiveAESink(CEvent *inMsgEvent);
  ~CActiveAESink();
  //! the scheduled start (CSampleBuffer::landEpoch) the output last landed
  unsigned int GetCommittedStart() const { return m_committedStart.load(); }

  void EnumerateSinkList(bool force, std::string driver);
  void EnumerateOutputDevices(AEDeviceList &devices, bool passthrough);
  std::string ValidateOuputDevice(const std::string& device, bool passthrough) const;
  void Start();
  void Dispose();
  AEDeviceType GetDeviceType(const std::string &device);
  bool HasPassthroughDevice();
  bool SupportsFormat(const std::string &device, AEAudioFormat &format);
  bool DeviceExist(std::string driver, const std::string& device);
  bool NeedIecPack() const { return m_needIecPack; }
  CSinkControlProtocol m_controlPort;
  CSinkDataProtocol m_dataPort;

protected:
  void OnStartup() override;
  void Process() override;
  void OnExit() override;
  void StateMachine(int signal, Protocol* port, Message* msg);
  void PrintSinks(std::string& driver);
  void GetDeviceFriendlyName(const std::string& device);
  void OpenSink();
  void CloseSink(bool drain = true);
  void ReturnBuffers();
  void SetSilenceTimer();
  bool NeedIECPacking();

  unsigned int OutputSamples(CSampleBuffer* samples);
  void ShadowOnPins(CSampleBuffer* samples, unsigned int writtenFrames, const AEDelayStatus& status);
  std::atomic<unsigned int> m_committedStart{0};
  int m_landDiag = 40; // TEMP LANDDIAG
  bool m_shadowAudible = false;
  void SwapInit(CSampleBuffer* samples);
  //! a scheduled start: pad the output so this buffer's first sample leaves at
  //! its landNs; false if it can no longer land (it is then dropped)
  bool LandScheduled(CSampleBuffer* samples);
  //! write raw packed frames from the packer, retrying a full device
  bool WritePacked(unsigned int frames);

  void GenerateNoise();

  CEvent m_outMsgEvent;
  CEvent *m_inMsgEvent;
  int m_state;
  bool m_bStateMachineSelfTrigger;
  std::chrono::milliseconds m_extTimeout;
  std::chrono::minutes m_silenceTimeOut{std::chrono::minutes::zero()};
  bool m_extError;
  std::chrono::milliseconds m_extSilenceTimeout;
  bool m_extAppFocused;
  bool m_extStreaming;
  //! a held session: pause bursts even before this sink has sent audio
  bool m_extHoldBursts = false;
  bool m_extReserved{false};
  std::chrono::duration<double> m_reservedCacheTotal{};
  std::chrono::duration<double> m_reservedLatency{};
  XbmcThreads::EndTime<> m_extSilenceTimer;

  CSampleBuffer m_sampleOfSilence;
  enum
  {
    CHECK_SWAP,
    NEED_CONVERT,
    NEED_BYTESWAP,
    SKIP_SWAP,
  } m_swapState;

  std::string m_deviceFriendlyName;
  std::string m_device;
  // Guards m_sinkInfoList only. EnumerateSinkList() rebuilds the list on the sink
  // thread while readers reach it straight from other threads - the settings GUI
  // calls EnumerateOutputDevices() with no message hop - so the vector must never
  // be enumerated while it is being rewritten. Never held across sink probing.
  mutable CCriticalSection m_sinkInfoLock;
  std::vector<AE::AESinkInfo> m_sinkInfoList;
  std::unique_ptr<IAESink> m_sink;
  AEAudioFormat m_sinkFormat, m_requestedFormat;
  CEngineStats *m_stats;
  float m_volume;
  int m_sinkLatency;
  std::unique_ptr<CAEBitstreamPacker> m_packer;
  bool m_needIecPack{false};
  bool m_streamNoise;
};

}
