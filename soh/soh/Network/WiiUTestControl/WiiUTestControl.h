#pragma once

#if defined(__WIIU__) && defined(SOH_WIIU_DEBUG_TELEMETRY)

#include <SDL2/SDL_net.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

class WiiUTestControl {
  public:
    static WiiUTestControl* Instance;

    bool Start(uint16_t port = 43385);
    void Stop();
    void Process();
    void NotifyTransitionEnd();

  private:
    static constexpr size_t MaxPacketSize = 64 * 1024;

    TCPsocket serverSocket = nullptr;
    TCPsocket clientSocket = nullptr;
    SDLNet_SocketSet socketSet = nullptr;
    std::string receivedData;
    uint16_t listenPort = 0;

    std::optional<nlohmann::json> pendingTransitionRequestId;
    std::optional<nlohmann::json> pendingRuntimeState;
    bool transitionCompleted = false;
    bool pendingTransitionClientConnected = false;

    void AcceptClient();
    void CloseClient();
    void ReceivePackets();
    void HandlePacket(const std::string& packet);
    void SendJson(const nlohmann::json& payload);
    void FinishPendingTransition();

    nlohmann::json CaptureState() const;
    bool ApplyState(const nlohmann::json& state, std::string& error, bool& waitingForTransition);
    bool ApplyRuntimeState(const nlohmann::json& runtime, std::string& error);
};

#endif
