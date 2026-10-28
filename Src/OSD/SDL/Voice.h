/**
 ** Voice chat between the two headsets of a two-player session.
 **
 ** The microphone is read 20 ms at a time (16 kHz, mono), squeezed to one byte per sample (mu-law,
 ** 128 kbit/s) and sent as 'V' messages on the game connection; what arrives is played on a second
 ** audio device. Quiet stretches are not sent. Voice never touches the game's state, so it cannot
 ** upset the lockstep.
 **/

#ifndef INCLUDED_VOICE_H
#define INCLUDED_VOICE_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include <SDL.h>

class VoiceChat
{
public:
  static constexpr int kRate = 16000;
  static constexpr int kFrame = 320;            // samples per packet (20 ms)

  std::function<void(const uint8_t *, uint32_t)> send;   // sends one packet to the other headset
  int threshold = 600;                          // microphone level (0..32767) that counts as talking
  std::atomic<bool> muted{false};

  ~VoiceChat() { Stop(); }

  bool Running() const { return running; }
  bool Talking() const { return talkingNow; }
  bool FriendTalking() const
  {
    return remoteTicks.load() && (SDL_GetTicks() - remoteTicks.load()) < 400;
  }

  // false: the microphone could not be opened (no permission yet, or none there)
  bool Start()
  {
    if (running)
      return true;
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = kFrame;
    SDL_AudioSpec have;
    cap = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
    if (!cap)
      return false;
    play = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!play)
    {
      SDL_CloseAudioDevice(cap);
      cap = 0;
      return false;
    }
    SDL_PauseAudioDevice(cap, 0);
    SDL_PauseAudioDevice(play, 1);              // started once a little has arrived
    stop = false;
    running = true;
    th = std::thread([this] { Loop(); });
    return true;
  }

  void Stop()
  {
    if (!running)
      return;
    stop = true;
    if (th.joinable())
      th.join();
    running = false;
    SDL_CloseAudioDevice(cap);
    SDL_CloseAudioDevice(play);
    cap = play = 0;
  }

  // A packet from the other headset (any thread)
  void OnPacket(const uint8_t *p, uint32_t n)
  {
    if (!running || !play || n == 0 || n > 4000)
      return;
    int16_t pcm[4000];
    for (uint32_t i = 0; i < n; i++)
      pcm[i] = Decode(p[i]);
    // keep the delay short: if a lot has piled up, drop it and carry on from here
    if (SDL_GetQueuedAudioSize(play) > (Uint32)(kRate / 4 * 2))
      SDL_ClearQueuedAudio(play);
    SDL_QueueAudio(play, pcm, n * 2);
    remoteTicks = SDL_GetTicks() | 1;
    if (SDL_GetAudioDeviceStatus(play) == SDL_AUDIO_PAUSED && SDL_GetQueuedAudioSize(play) >= (Uint32)(kFrame * 2 * 3))
      SDL_PauseAudioDevice(play, 0);            // enough ahead to play smoothly
  }

private:
  SDL_AudioDeviceID cap = 0, play = 0;
  std::thread th;
  std::atomic<bool> stop{false}, running{false}, talkingNow{false};
  std::atomic<uint32_t> remoteTicks{0};

  static uint8_t Encode(int16_t s)
  {
    int sign = (s >> 8) & 0x80;
    int v = s < 0 ? -(int)s : s;
    if (v > 32635) v = 32635;
    v += 0x84;
    int exp = 7;
    for (int mask = 0x4000; (v & mask) == 0 && exp > 0; exp--, mask >>= 1) {}
    const int mant = (v >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
  }
  static int16_t Decode(uint8_t u)
  {
    u = (uint8_t)~u;
    const int sign = u & 0x80, exp = (u >> 4) & 7, mant = u & 15;
    int v = (((mant << 3) + 0x84) << exp) - 0x84;
    return (int16_t)(sign ? -v : v);
  }

  void Loop()
  {
    uint32_t lastLoud = 0;
    while (!stop)
    {
      const Uint32 want = kFrame * 2;
      Uint32 have = SDL_GetQueuedAudioSize(cap);
      if (have > want * 20)                     // far behind (the app stalled): throw the old sound away
      {
        SDL_ClearQueuedAudio(cap);
        have = 0;
      }
      while (have >= want && !stop)
      {
        int16_t pcm[kFrame];
        SDL_DequeueAudio(cap, pcm, want);
        have -= want;
        double sum = 0;
        for (int i = 0; i < kFrame; i++)
          sum += (double)pcm[i] * pcm[i];
        const int level = (int)std::sqrt(sum / kFrame);
        const uint32_t now = SDL_GetTicks();
        if (level >= threshold)
          lastLoud = now | 1;
        const bool talking = !muted && lastLoud && (now - lastLoud) < 350;   // a little tail so words are not clipped
        talkingNow = talking;
        if (talking && send)
        {
          uint8_t out[kFrame];
          for (int i = 0; i < kFrame; i++)
            out[i] = Encode(pcm[i]);
          send(out, kFrame);
        }
      }
      // the other side stopped talking and what was queued has played: wait for a fresh lead next time
      if (SDL_GetQueuedAudioSize(play) == 0 && SDL_GetAudioDeviceStatus(play) == SDL_AUDIO_PLAYING)
        SDL_PauseAudioDevice(play, 1);
      SDL_Delay(8);
    }
  }
};

#endif
