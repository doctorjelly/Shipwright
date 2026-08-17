#include "WiiUTestControl.h"

#if defined(__WIIU__) && defined(SOH_WIIU_DEBUG_TELEMETRY)

#include <coreinit/debug.h>

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "soh/Enhancements/savestates.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
}

extern "C" PlayState* gPlayState;

namespace {
using json = nlohmann::json;

constexpr uint32_t ProtocolVersion = 1;
constexpr size_t SceneFlagCount = 124;
static_assert(ARRAY_COUNT(gSaveContext.sceneFlags) == SceneFlagCount);
static_assert(ARRAY_COUNT(gSaveContext.gsFlags) == 6);
static_assert(ARRAY_COUNT(gSaveContext.eventChkInf) == 14);
static_assert(ARRAY_COUNT(gSaveContext.itemGetInf) == 4);
static_assert(ARRAY_COUNT(gSaveContext.infTable) == 30);

struct ParsedPersistentState {
    std::optional<s32> entranceIndex;
    std::optional<s32> linkAge;
    std::optional<s32> cutsceneIndex;
    std::optional<u16> dayTime;
    std::optional<s32> nightFlag;
    std::optional<s16> healthCapacity;
    std::optional<s16> health;
    std::optional<s8> magicLevel;
    std::optional<s8> magic;
    std::optional<s16> magicCapacity;
    std::optional<s16> rupees;
    std::optional<u16> swordHealth;
    std::optional<u8> isMagicAcquired;
    std::optional<u8> isDoubleMagicAcquired;
    std::optional<u8> isDoubleDefenseAcquired;
    std::optional<u8> bgsFlag;
    std::optional<s16> savedSceneNum;
    std::optional<u32> worldMapAreaData;
    std::optional<u8> questId;
    std::optional<ItemEquips> childEquips;
    std::optional<ItemEquips> adultEquips;
    std::optional<ItemEquips> equips;
    std::optional<Inventory> inventory;
    std::optional<std::array<SavedSceneFlags, SceneFlagCount>> sceneFlags;
    std::optional<std::array<u32, 6>> gsFlags;
    std::optional<std::array<u16, 14>> eventChkInf;
    std::optional<std::array<u16, 4>> itemGetInf;
    std::optional<std::array<u16, 30>> infTable;
};

struct ParsedRuntimeState {
    std::optional<s16> sceneNum;
    std::optional<s8> roomNum;
    std::optional<Vec3f> position;
    std::optional<Vec3s> worldRotation;
    std::optional<Vec3s> shapeRotation;
    std::optional<SavedSceneFlags> sceneFlags;
    std::optional<u32> tempSwch;
    std::optional<u32> tempCollect;
};

template <typename T, size_t N> json NumericArrayToJson(const T (&values)[N]) {
    json result = json::array();
    for (const auto value : values) {
        result.push_back(static_cast<int64_t>(value));
    }
    return result;
}

json EquipsToJson(const ItemEquips& equips) {
    return {
        { "buttonItems", NumericArrayToJson(equips.buttonItems) },
        { "cButtonSlots", NumericArrayToJson(equips.cButtonSlots) },
        { "equipment", equips.equipment },
    };
}

json InventoryToJson(const Inventory& inventory) {
    return {
        { "items", NumericArrayToJson(inventory.items) },
        { "ammo", NumericArrayToJson(inventory.ammo) },
        { "equipment", inventory.equipment },
        { "upgrades", inventory.upgrades },
        { "questItems", inventory.questItems },
        { "dungeonItems", NumericArrayToJson(inventory.dungeonItems) },
        { "dungeonKeys", NumericArrayToJson(inventory.dungeonKeys) },
        { "defenseHearts", inventory.defenseHearts },
        { "gsTokens", inventory.gsTokens },
    };
}

json SceneFlagsToJson(const SavedSceneFlags& flags) {
    return {
        { "chest", flags.chest }, { "swch", flags.swch },   { "clear", flags.clear },   { "collect", flags.collect },
        { "unk", flags.unk },     { "rooms", flags.rooms }, { "floors", flags.floors },
    };
}

json RuntimeSceneFlagsToJson(const SavedSceneFlags& flags) {
    return {
        { "chest", flags.chest },
        { "swch", flags.swch },
        { "clear", flags.clear },
        { "collect", flags.collect },
    };
}

bool ReadIntegerValue(const json& value, int64_t minimum, int64_t maximum, int64_t& output, std::string& error,
                      const std::string& path) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        error = path + " must be an integer";
        return false;
    }

    if (value.is_number_unsigned()) {
        const uint64_t unsignedValue = value.get<uint64_t>();
        if (unsignedValue > static_cast<uint64_t>(maximum)) {
            error = path + " is outside the supported range";
            return false;
        }
        output = static_cast<int64_t>(unsignedValue);
    } else {
        output = value.get<int64_t>();
        if (output < minimum || output > maximum) {
            error = path + " is outside the supported range";
            return false;
        }
    }

    if (output < minimum || output > maximum) {
        error = path + " is outside the supported range";
        return false;
    }
    return true;
}

template <typename T>
bool ReadOptionalInteger(const json& object, const char* key, int64_t minimum, int64_t maximum,
                         std::optional<T>& output, std::string& error, const std::string& prefix) {
    if (!object.contains(key)) {
        return true;
    }
    int64_t value = 0;
    if (!ReadIntegerValue(object.at(key), minimum, maximum, value, error, prefix + key)) {
        return false;
    }
    output = static_cast<T>(value);
    return true;
}

template <typename T>
bool ReadIntegerMember(const json& object, const char* key, int64_t minimum, int64_t maximum, T& output,
                       std::string& error, const std::string& prefix) {
    if (!object.contains(key)) {
        return true;
    }
    int64_t value = 0;
    if (!ReadIntegerValue(object.at(key), minimum, maximum, value, error, prefix + key)) {
        return false;
    }
    output = static_cast<T>(value);
    return true;
}

template <typename T, size_t N>
bool ReadFixedArray(const json& value, T (&output)[N], int64_t minimum, int64_t maximum, std::string& error,
                    const std::string& path) {
    if (!value.is_array() || value.size() != N) {
        error = path + " must contain exactly " + std::to_string(N) + " integers";
        return false;
    }
    for (size_t i = 0; i < N; ++i) {
        int64_t parsed = 0;
        if (!ReadIntegerValue(value.at(i), minimum, maximum, parsed, error, path + "[" + std::to_string(i) + "]")) {
            return false;
        }
        output[i] = static_cast<T>(parsed);
    }
    return true;
}

template <typename T, size_t N>
bool ReadFixedArray(const json& value, std::array<T, N>& output, int64_t minimum, int64_t maximum, std::string& error,
                    const std::string& path) {
    if (!value.is_array() || value.size() != N) {
        error = path + " must contain exactly " + std::to_string(N) + " integers";
        return false;
    }
    for (size_t i = 0; i < N; ++i) {
        int64_t parsed = 0;
        if (!ReadIntegerValue(value.at(i), minimum, maximum, parsed, error, path + "[" + std::to_string(i) + "]")) {
            return false;
        }
        output[i] = static_cast<T>(parsed);
    }
    return true;
}

bool ParseEquips(const json& value, ItemEquips& output, std::string& error, const std::string& path) {
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }
    if (value.contains("buttonItems") &&
        !ReadFixedArray(value.at("buttonItems"), output.buttonItems, 0, UINT8_MAX, error, path + ".buttonItems")) {
        return false;
    }
    if (value.contains("cButtonSlots") &&
        !ReadFixedArray(value.at("cButtonSlots"), output.cButtonSlots, 0, UINT8_MAX, error, path + ".cButtonSlots")) {
        return false;
    }
    return ReadIntegerMember(value, "equipment", 0, UINT16_MAX, output.equipment, error, path + ".");
}

bool ParseInventory(const json& value, Inventory& output, std::string& error) {
    constexpr const char* path = "state.save.inventory";
    if (!value.is_object()) {
        error = std::string(path) + " must be an object";
        return false;
    }
    if (value.contains("items") &&
        !ReadFixedArray(value.at("items"), output.items, 0, UINT8_MAX, error, std::string(path) + ".items")) {
        return false;
    }
    if (value.contains("ammo") &&
        !ReadFixedArray(value.at("ammo"), output.ammo, -1, INT8_MAX, error, std::string(path) + ".ammo")) {
        return false;
    }
    if (value.contains("dungeonItems") && !ReadFixedArray(value.at("dungeonItems"), output.dungeonItems, 0, UINT8_MAX,
                                                          error, std::string(path) + ".dungeonItems")) {
        return false;
    }
    if (value.contains("dungeonKeys") && !ReadFixedArray(value.at("dungeonKeys"), output.dungeonKeys, -1, INT8_MAX,
                                                         error, std::string(path) + ".dungeonKeys")) {
        return false;
    }
    return ReadIntegerMember(value, "equipment", 0, UINT16_MAX, output.equipment, error, std::string(path) + ".") &&
           ReadIntegerMember(value, "upgrades", 0, UINT32_MAX, output.upgrades, error, std::string(path) + ".") &&
           ReadIntegerMember(value, "questItems", 0, UINT32_MAX, output.questItems, error, std::string(path) + ".") &&
           ReadIntegerMember(value, "defenseHearts", 0, INT8_MAX, output.defenseHearts, error,
                             std::string(path) + ".") &&
           ReadIntegerMember(value, "gsTokens", 0, INT16_MAX, output.gsTokens, error, std::string(path) + ".");
}

bool ParseSceneFlagsObject(const json& value, SavedSceneFlags& output, std::string& error, const std::string& path) {
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }
    return ReadIntegerMember(value, "chest", 0, UINT32_MAX, output.chest, error, path + ".") &&
           ReadIntegerMember(value, "swch", 0, UINT32_MAX, output.swch, error, path + ".") &&
           ReadIntegerMember(value, "clear", 0, UINT32_MAX, output.clear, error, path + ".") &&
           ReadIntegerMember(value, "collect", 0, UINT32_MAX, output.collect, error, path + ".") &&
           ReadIntegerMember(value, "unk", 0, UINT32_MAX, output.unk, error, path + ".") &&
           ReadIntegerMember(value, "rooms", 0, UINT32_MAX, output.rooms, error, path + ".") &&
           ReadIntegerMember(value, "floors", 0, UINT32_MAX, output.floors, error, path + ".");
}

bool ReadFloatMember(const json& object, const char* key, float& output, std::string& error,
                     const std::string& prefix) {
    if (!object.contains(key) || !object.at(key).is_number()) {
        error = prefix + key + " must be a number";
        return false;
    }
    const double value = object.at(key).get<double>();
    if (!std::isfinite(value) || value < -1000000.0 || value > 1000000.0) {
        error = prefix + key + " is outside the supported range";
        return false;
    }
    output = static_cast<float>(value);
    return true;
}

bool ParseVec3f(const json& value, Vec3f& output, std::string& error, const std::string& path) {
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }
    return ReadFloatMember(value, "x", output.x, error, path + ".") &&
           ReadFloatMember(value, "y", output.y, error, path + ".") &&
           ReadFloatMember(value, "z", output.z, error, path + ".");
}

bool ParseVec3s(const json& value, Vec3s& output, std::string& error, const std::string& path) {
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }
    return ReadIntegerMember(value, "x", INT16_MIN, INT16_MAX, output.x, error, path + ".") &&
           ReadIntegerMember(value, "y", INT16_MIN, INT16_MAX, output.y, error, path + ".") &&
           ReadIntegerMember(value, "z", INT16_MIN, INT16_MAX, output.z, error, path + ".");
}

bool ParseRuntimeState(const json& value, ParsedRuntimeState& output, std::string& error) {
    if (!value.is_object()) {
        error = "state.runtime must be an object";
        return false;
    }
    if (!ReadOptionalInteger(value, "sceneNum", 0, SCENE_ID_MAX - 1, output.sceneNum, error, "state.runtime.") ||
        !ReadOptionalInteger(value, "roomNum", -1, INT8_MAX, output.roomNum, error, "state.runtime.")) {
        return false;
    }
    if (value.contains("position")) {
        Vec3f position{};
        if (!ParseVec3f(value.at("position"), position, error, "state.runtime.position")) {
            return false;
        }
        output.position = position;
    }
    if (value.contains("worldRotation")) {
        Vec3s rotation{};
        if (!ParseVec3s(value.at("worldRotation"), rotation, error, "state.runtime.worldRotation")) {
            return false;
        }
        output.worldRotation = rotation;
    }
    if (value.contains("shapeRotation")) {
        Vec3s rotation{};
        if (!ParseVec3s(value.at("shapeRotation"), rotation, error, "state.runtime.shapeRotation")) {
            return false;
        }
        output.shapeRotation = rotation;
    }
    if (value.contains("sceneFlags")) {
        SavedSceneFlags flags{};
        if (!ParseSceneFlagsObject(value.at("sceneFlags"), flags, error, "state.runtime.sceneFlags")) {
            return false;
        }
        output.sceneFlags = flags;
    }
    if (!ReadOptionalInteger(value, "tempSwch", 0, UINT32_MAX, output.tempSwch, error, "state.runtime.") ||
        !ReadOptionalInteger(value, "tempCollect", 0, UINT32_MAX, output.tempCollect, error, "state.runtime.")) {
        return false;
    }
    return true;
}

bool ParsePersistentState(const json& value, ParsedPersistentState& output, std::string& error) {
    if (!value.is_object()) {
        error = "state.save must be an object";
        return false;
    }

    if (!ReadOptionalInteger(value, "entranceIndex", 0, ENTR_MAX - 1, output.entranceIndex, error, "state.save.") ||
        !ReadOptionalInteger(value, "linkAge", LINK_AGE_ADULT, LINK_AGE_CHILD, output.linkAge, error, "state.save.") ||
        !ReadOptionalInteger(value, "cutsceneIndex", 0, UINT16_MAX, output.cutsceneIndex, error, "state.save.") ||
        !ReadOptionalInteger(value, "dayTime", 0, UINT16_MAX, output.dayTime, error, "state.save.") ||
        !ReadOptionalInteger(value, "nightFlag", 0, 1, output.nightFlag, error, "state.save.") ||
        !ReadOptionalInteger(value, "healthCapacity", 0, MAX_HEALTH, output.healthCapacity, error, "state.save.") ||
        !ReadOptionalInteger(value, "health", 0, MAX_HEALTH, output.health, error, "state.save.") ||
        !ReadOptionalInteger(value, "magicLevel", 0, 2, output.magicLevel, error, "state.save.") ||
        !ReadOptionalInteger(value, "magic", 0, MAGIC_DOUBLE_METER, output.magic, error, "state.save.") ||
        !ReadOptionalInteger(value, "magicCapacity", 0, MAGIC_DOUBLE_METER, output.magicCapacity, error,
                             "state.save.") ||
        !ReadOptionalInteger(value, "rupees", 0, 999, output.rupees, error, "state.save.") ||
        !ReadOptionalInteger(value, "swordHealth", 0, UINT16_MAX, output.swordHealth, error, "state.save.") ||
        !ReadOptionalInteger(value, "isMagicAcquired", 0, 1, output.isMagicAcquired, error, "state.save.") ||
        !ReadOptionalInteger(value, "isDoubleMagicAcquired", 0, 1, output.isDoubleMagicAcquired, error,
                             "state.save.") ||
        !ReadOptionalInteger(value, "isDoubleDefenseAcquired", 0, 1, output.isDoubleDefenseAcquired, error,
                             "state.save.") ||
        !ReadOptionalInteger(value, "bgsFlag", 0, 1, output.bgsFlag, error, "state.save.") ||
        !ReadOptionalInteger(value, "savedSceneNum", -1, SCENE_ID_MAX - 1, output.savedSceneNum, error,
                             "state.save.") ||
        !ReadOptionalInteger(value, "worldMapAreaData", 0, UINT32_MAX, output.worldMapAreaData, error, "state.save.") ||
        !ReadOptionalInteger(value, "questId", QUEST_NORMAL, QUEST_BOSSRUSH, output.questId, error, "state.save.")) {
        return false;
    }

    if (value.contains("childEquips")) {
        ItemEquips equips = gSaveContext.childEquips;
        if (!ParseEquips(value.at("childEquips"), equips, error, "state.save.childEquips")) {
            return false;
        }
        output.childEquips = equips;
    }
    if (value.contains("adultEquips")) {
        ItemEquips equips = gSaveContext.adultEquips;
        if (!ParseEquips(value.at("adultEquips"), equips, error, "state.save.adultEquips")) {
            return false;
        }
        output.adultEquips = equips;
    }
    if (value.contains("equips")) {
        ItemEquips equips = gSaveContext.equips;
        if (!ParseEquips(value.at("equips"), equips, error, "state.save.equips")) {
            return false;
        }
        output.equips = equips;
    }
    if (value.contains("inventory")) {
        Inventory inventory = gSaveContext.inventory;
        if (!ParseInventory(value.at("inventory"), inventory, error)) {
            return false;
        }
        output.inventory = inventory;
    }
    if (value.contains("sceneFlags")) {
        const json& sceneFlags = value.at("sceneFlags");
        if (!sceneFlags.is_array() || sceneFlags.size() != SceneFlagCount) {
            error = "state.save.sceneFlags must contain exactly 124 objects";
            return false;
        }
        std::array<SavedSceneFlags, SceneFlagCount> parsed{};
        for (size_t i = 0; i < SceneFlagCount; ++i) {
            parsed[i] = gSaveContext.sceneFlags[i];
            if (!ParseSceneFlagsObject(sceneFlags.at(i), parsed[i], error,
                                       "state.save.sceneFlags[" + std::to_string(i) + "]")) {
                return false;
            }
        }
        output.sceneFlags = parsed;
    }
    if (value.contains("gsFlags")) {
        std::array<u32, 6> parsed{};
        if (!ReadFixedArray(value.at("gsFlags"), parsed, 0, UINT32_MAX, error, "state.save.gsFlags")) {
            return false;
        }
        output.gsFlags = parsed;
    }
    if (value.contains("eventChkInf")) {
        std::array<u16, 14> parsed{};
        if (!ReadFixedArray(value.at("eventChkInf"), parsed, 0, UINT16_MAX, error, "state.save.eventChkInf")) {
            return false;
        }
        output.eventChkInf = parsed;
    }
    if (value.contains("itemGetInf")) {
        std::array<u16, 4> parsed{};
        if (!ReadFixedArray(value.at("itemGetInf"), parsed, 0, UINT16_MAX, error, "state.save.itemGetInf")) {
            return false;
        }
        output.itemGetInf = parsed;
    }
    if (value.contains("infTable")) {
        std::array<u16, 30> parsed{};
        if (!ReadFixedArray(value.at("infTable"), parsed, 0, UINT16_MAX, error, "state.save.infTable")) {
            return false;
        }
        output.infTable = parsed;
    }
    return true;
}

const char* SaveStateResultName(SaveStateReturn result) {
    switch (result) {
        case SaveStateReturn::SUCCESS:
            return "queued";
        case SaveStateReturn::FAIL_INVALID_SLOT:
            return "invalid_or_empty_slot";
        case SaveStateReturn::FAIL_NO_MEMORY:
            return "out_of_memory";
        case SaveStateReturn::FAIL_STATE_EMPTY:
            return "empty_slot";
        case SaveStateReturn::FAIL_WRONG_GAMESTATE:
            return "no_game_loaded";
        case SaveStateReturn::FAIL_BAD_REQUEST:
        default:
            return "bad_request";
    }
}
} // namespace

WiiUTestControl* WiiUTestControl::Instance = nullptr;

bool WiiUTestControl::Start(uint16_t port) {
    if (serverSocket != nullptr) {
        return true;
    }

    IPaddress address{};
    if (SDLNet_ResolveHost(&address, nullptr, port) < 0) {
        OSReport("[SoH][test-control] resolve failed: %s\n", SDLNet_GetError());
        return false;
    }
    serverSocket = SDLNet_TCP_Open(&address);
    if (serverSocket == nullptr) {
        OSReport("[SoH][test-control] listen failed: %s\n", SDLNet_GetError());
        return false;
    }
    socketSet = SDLNet_AllocSocketSet(2);
    if (socketSet == nullptr) {
        OSReport("[SoH][test-control] socket-set allocation failed: %s\n", SDLNet_GetError());
        SDLNet_TCP_Close(serverSocket);
        serverSocket = nullptr;
        return false;
    }
    if (SDLNet_TCP_AddSocket(socketSet, serverSocket) < 0) {
        OSReport("[SoH][test-control] add server failed: %s\n", SDLNet_GetError());
        SDLNet_FreeSocketSet(socketSet);
        socketSet = nullptr;
        SDLNet_TCP_Close(serverSocket);
        serverSocket = nullptr;
        return false;
    }

    listenPort = port;
    OSReport("[SoH][test-control] listening tcp=%u schema=%u\n", static_cast<unsigned int>(listenPort),
             static_cast<unsigned int>(ProtocolVersion));
    return true;
}

void WiiUTestControl::Stop() {
    CloseClient();
    if (serverSocket != nullptr) {
        if (socketSet != nullptr) {
            SDLNet_TCP_DelSocket(socketSet, serverSocket);
        }
        SDLNet_TCP_Close(serverSocket);
        serverSocket = nullptr;
    }
    if (socketSet != nullptr) {
        SDLNet_FreeSocketSet(socketSet);
        socketSet = nullptr;
    }
    pendingTransitionRequestId.reset();
    pendingRuntimeState.reset();
    transitionCompleted = false;
    pendingTransitionClientConnected = false;
    listenPort = 0;
}

void WiiUTestControl::AcceptClient() {
    if (serverSocket == nullptr || clientSocket != nullptr || !SDLNet_SocketReady(serverSocket)) {
        return;
    }
    clientSocket = SDLNet_TCP_Accept(serverSocket);
    if (clientSocket == nullptr) {
        return;
    }
    if (SDLNet_TCP_AddSocket(socketSet, clientSocket) < 0) {
        OSReport("[SoH][test-control] add client failed: %s\n", SDLNet_GetError());
        SDLNet_TCP_Close(clientSocket);
        clientSocket = nullptr;
        return;
    }
    receivedData.clear();
    OSReport("[SoH][test-control] client connected\n");
    SendJson({ { "schema", ProtocolVersion },
               { "type", "hello" },
               { "service", "soh-wiiu-test-control" },
               { "version", std::string(gBuildVersion) },
               { "commit", std::string(gGitCommitHash) } });
}

void WiiUTestControl::CloseClient() {
    if (clientSocket == nullptr) {
        return;
    }
    if (socketSet != nullptr) {
        SDLNet_TCP_DelSocket(socketSet, clientSocket);
    }
    SDLNet_TCP_Close(clientSocket);
    clientSocket = nullptr;
    receivedData.clear();
    if (pendingTransitionRequestId.has_value()) {
        pendingTransitionClientConnected = false;
    }
    OSReport("[SoH][test-control] client disconnected\n");
}

void WiiUTestControl::SendJson(const json& payload) {
    if (clientSocket == nullptr) {
        return;
    }
    std::string encoded = payload.dump();
    encoded.push_back('\0');
    if (encoded.size() > MaxPacketSize) {
        OSReport("[SoH][test-control] response exceeds packet limit\n");
        json error = {
            { "schema", ProtocolVersion },
            { "type", "result" },
            { "id", payload.contains("id") ? payload.at("id") : json(nullptr) },
            { "status", "failure" },
            { "error", "response exceeds the test-control packet limit" },
        };
        encoded = error.dump();
        encoded.push_back('\0');
    }
    size_t sent = 0;
    while (sent < encoded.size()) {
        const int result =
            SDLNet_TCP_Send(clientSocket, encoded.data() + sent, static_cast<int>(encoded.size() - sent));
        if (result <= 0) {
            OSReport("[SoH][test-control] send failed: %s\n", SDLNet_GetError());
            CloseClient();
            return;
        }
        sent += static_cast<size_t>(result);
    }
}

void WiiUTestControl::ReceivePackets() {
    if (clientSocket == nullptr || socketSet == nullptr || !SDLNet_SocketReady(clientSocket)) {
        return;
    }

    char buffer[4096];
    const int received = SDLNet_TCP_Recv(clientSocket, buffer, static_cast<int>(sizeof(buffer)));
    if (received <= 0) {
        CloseClient();
        return;
    }
    receivedData.append(buffer, static_cast<size_t>(received));
    if (receivedData.size() > MaxPacketSize) {
        OSReport("[SoH][test-control] request exceeds packet limit\n");
        CloseClient();
        return;
    }

    size_t delimiter = receivedData.find('\0');
    while (delimiter != std::string::npos) {
        std::string packet = receivedData.substr(0, delimiter);
        receivedData.erase(0, delimiter + 1);
        HandlePacket(packet);
        if (clientSocket == nullptr) {
            return;
        }
        delimiter = receivedData.find('\0');
    }
}

void WiiUTestControl::Process() {
    if (transitionCompleted) {
        FinishPendingTransition();
    }
    if (socketSet == nullptr) {
        return;
    }
    const int ready = SDLNet_CheckSockets(socketSet, 0);
    if (ready < 0) {
        OSReport("[SoH][test-control] poll failed: %s\n", SDLNet_GetError());
        CloseClient();
        return;
    }
    if (ready == 0) {
        return;
    }
    AcceptClient();
    ReceivePackets();
}

void WiiUTestControl::NotifyTransitionEnd() {
    if (pendingTransitionRequestId.has_value()) {
        transitionCompleted = true;
    }
}

json WiiUTestControl::CaptureState() const {
    const bool saveLoaded = GameInteractor::IsSaveLoaded(true);
    json state = {
        { "schema", ProtocolVersion },
        { "version", std::string(gBuildVersion) },
        { "commit", std::string(gGitCommitHash) },
        { "saveLoaded", saveLoaded },
    };
    if (!saveLoaded) {
        return state;
    }

    json save = {
        { "entranceIndex", gSaveContext.entranceIndex },
        { "linkAge", gSaveContext.linkAge },
        { "cutsceneIndex", gSaveContext.cutsceneIndex },
        { "dayTime", gSaveContext.dayTime },
        { "nightFlag", gSaveContext.nightFlag },
        { "healthCapacity", gSaveContext.healthCapacity },
        { "health", gSaveContext.health },
        { "magicLevel", gSaveContext.magicLevel },
        { "magic", gSaveContext.magic },
        { "magicCapacity", gSaveContext.magicCapacity },
        { "rupees", gSaveContext.rupees },
        { "swordHealth", gSaveContext.swordHealth },
        { "isMagicAcquired", gSaveContext.isMagicAcquired },
        { "isDoubleMagicAcquired", gSaveContext.isDoubleMagicAcquired },
        { "isDoubleDefenseAcquired", gSaveContext.isDoubleDefenseAcquired },
        { "bgsFlag", gSaveContext.bgsFlag },
        { "savedSceneNum", gSaveContext.savedSceneNum },
        { "worldMapAreaData", gSaveContext.worldMapAreaData },
        { "questId", gSaveContext.ship.quest.id },
        { "childEquips", EquipsToJson(gSaveContext.childEquips) },
        { "adultEquips", EquipsToJson(gSaveContext.adultEquips) },
        { "equips", EquipsToJson(gSaveContext.equips) },
        { "inventory", InventoryToJson(gSaveContext.inventory) },
        { "eventChkInf", NumericArrayToJson(gSaveContext.eventChkInf) },
        { "itemGetInf", NumericArrayToJson(gSaveContext.itemGetInf) },
        { "infTable", NumericArrayToJson(gSaveContext.infTable) },
    };

    json gsFlags = json::array();
    for (const s32 flag : gSaveContext.gsFlags) {
        gsFlags.push_back(static_cast<u32>(flag));
    }
    save["gsFlags"] = std::move(gsFlags);

    json sceneFlags = json::array();
    for (const auto& flags : gSaveContext.sceneFlags) {
        sceneFlags.push_back(SceneFlagsToJson(flags));
    }
    save["sceneFlags"] = std::move(sceneFlags);
    state["save"] = std::move(save);

    SavedSceneFlags runtimeSceneFlags{};
    runtimeSceneFlags.chest = gPlayState->actorCtx.flags.chest;
    runtimeSceneFlags.swch = gPlayState->actorCtx.flags.swch;
    runtimeSceneFlags.clear = gPlayState->actorCtx.flags.clear;
    runtimeSceneFlags.collect = gPlayState->actorCtx.flags.collect;
    json runtime = {
        { "sceneNum", gPlayState->sceneNum },
        { "roomNum", gPlayState->roomCtx.curRoom.num },
        { "gameplayFrames", gPlayState->gameplayFrames },
        { "sceneFlags", RuntimeSceneFlagsToJson(runtimeSceneFlags) },
        { "tempSwch", gPlayState->actorCtx.flags.tempSwch },
        { "tempCollect", gPlayState->actorCtx.flags.tempCollect },
    };
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr) {
        runtime["position"] = { { "x", player->actor.world.pos.x },
                                { "y", player->actor.world.pos.y },
                                { "z", player->actor.world.pos.z } };
        runtime["worldRotation"] = { { "x", player->actor.world.rot.x },
                                     { "y", player->actor.world.rot.y },
                                     { "z", player->actor.world.rot.z } };
        runtime["shapeRotation"] = { { "x", player->actor.shape.rot.x },
                                     { "y", player->actor.shape.rot.y },
                                     { "z", player->actor.shape.rot.z } };
    }
    state["runtime"] = std::move(runtime);
    return state;
}

bool WiiUTestControl::ApplyRuntimeState(const json& runtime, std::string& error) {
    if (!GameInteractor::IsSaveLoaded(true)) {
        error = "no game is loaded";
        return false;
    }
    ParsedRuntimeState parsed;
    if (!ParseRuntimeState(runtime, parsed, error)) {
        return false;
    }
    if (parsed.sceneNum.has_value() && *parsed.sceneNum != gPlayState->sceneNum) {
        error = "runtime scene does not match the loaded scene";
        return false;
    }
    if (parsed.roomNum.has_value() && *parsed.roomNum != gPlayState->roomCtx.curRoom.num) {
        error = "runtime room does not match the loaded room";
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if ((parsed.position.has_value() || parsed.worldRotation.has_value() || parsed.shapeRotation.has_value()) &&
        player == nullptr) {
        error = "the player actor is not available";
        return false;
    }
    if (parsed.position.has_value()) {
        player->actor.world.pos = *parsed.position;
        player->actor.prevPos = *parsed.position;
    }
    if (parsed.worldRotation.has_value()) {
        player->actor.world.rot = *parsed.worldRotation;
    }
    if (parsed.shapeRotation.has_value()) {
        player->actor.shape.rot = *parsed.shapeRotation;
    }
    if (parsed.sceneFlags.has_value()) {
        const auto& flags = *parsed.sceneFlags;
        gPlayState->actorCtx.flags.chest = flags.chest;
        gPlayState->actorCtx.flags.swch = flags.swch;
        gPlayState->actorCtx.flags.clear = flags.clear;
        gPlayState->actorCtx.flags.collect = flags.collect;
        if (gPlayState->sceneNum >= 0 && gPlayState->sceneNum < static_cast<s16>(SceneFlagCount)) {
            gSaveContext.sceneFlags[gPlayState->sceneNum].chest = flags.chest;
            gSaveContext.sceneFlags[gPlayState->sceneNum].swch = flags.swch;
            gSaveContext.sceneFlags[gPlayState->sceneNum].clear = flags.clear;
            gSaveContext.sceneFlags[gPlayState->sceneNum].collect = flags.collect;
        }
    }
    if (parsed.tempSwch.has_value()) {
        gPlayState->actorCtx.flags.tempSwch = *parsed.tempSwch;
    }
    if (parsed.tempCollect.has_value()) {
        gPlayState->actorCtx.flags.tempCollect = *parsed.tempCollect;
    }
    return true;
}

bool WiiUTestControl::ApplyState(const json& state, std::string& error, bool& waitingForTransition) {
    waitingForTransition = false;
    if (!GameInteractor::IsSaveLoaded(true)) {
        error = "no game is loaded";
        return false;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        error = "wait for the current scene transition to finish";
        return false;
    }
    if (GameInteractor::IsGameplayPaused()) {
        error = "unpause gameplay and close text boxes before applying state";
        return false;
    }
    if (!state.is_object() || (!state.contains("save") && !state.contains("runtime"))) {
        error = "state must contain a save or runtime object";
        return false;
    }
    if (state.contains("schema")) {
        int64_t schema = 0;
        if (!ReadIntegerValue(state.at("schema"), ProtocolVersion, ProtocolVersion, schema, error, "state.schema")) {
            return false;
        }
    }

    ParsedPersistentState persistent;
    ParsedRuntimeState runtimeValidation;
    if (state.contains("save") && !ParsePersistentState(state.at("save"), persistent, error)) {
        return false;
    }
    if (state.contains("runtime") && !ParseRuntimeState(state.at("runtime"), runtimeValidation, error)) {
        return false;
    }
    if (pendingTransitionRequestId.has_value()) {
        error = "another state transition is still pending";
        return false;
    }
    if (persistent.questId.has_value() && *persistent.questId != gSaveContext.ship.quest.id) {
        error = "state.save.questId must match the loaded save";
        return false;
    }
    if (!persistent.entranceIndex.has_value() && state.contains("runtime")) {
        if (runtimeValidation.sceneNum.has_value() && *runtimeValidation.sceneNum != gPlayState->sceneNum) {
            error = "runtime scene does not match the loaded scene";
            return false;
        }
        if (runtimeValidation.roomNum.has_value() && *runtimeValidation.roomNum != gPlayState->roomCtx.curRoom.num) {
            error = "runtime room does not match the loaded room";
            return false;
        }
        if ((runtimeValidation.position.has_value() || runtimeValidation.worldRotation.has_value() ||
             runtimeValidation.shapeRotation.has_value()) &&
            GET_PLAYER(gPlayState) == nullptr) {
            error = "the player actor is not available";
            return false;
        }
    }
    const s16 resultingHealthCapacity = persistent.healthCapacity.value_or(gSaveContext.healthCapacity);
    const s16 resultingHealth = persistent.health.value_or(gSaveContext.health);
    if (resultingHealth > resultingHealthCapacity) {
        error = "state.save.health cannot exceed state.save.healthCapacity";
        return false;
    }
    const bool resultingMagicAcquired = persistent.isMagicAcquired.value_or(gSaveContext.isMagicAcquired) != 0;
    const s16 resultingMagicCapacity = persistent.magicCapacity.value_or(gSaveContext.magicCapacity);
    const s16 resultingMagic = persistent.magic.value_or(gSaveContext.magic);
    if (resultingMagicAcquired && resultingMagic > resultingMagicCapacity) {
        error = "state.save.magic cannot exceed state.save.magicCapacity";
        return false;
    }

#define APPLY_OPTIONAL(field)                   \
    if (persistent.field.has_value()) {         \
        gSaveContext.field = *persistent.field; \
    }
    APPLY_OPTIONAL(entranceIndex)
    APPLY_OPTIONAL(linkAge)
    APPLY_OPTIONAL(cutsceneIndex)
    APPLY_OPTIONAL(dayTime)
    APPLY_OPTIONAL(nightFlag)
    APPLY_OPTIONAL(healthCapacity)
    APPLY_OPTIONAL(health)
    APPLY_OPTIONAL(magicLevel)
    APPLY_OPTIONAL(magic)
    APPLY_OPTIONAL(magicCapacity)
    APPLY_OPTIONAL(rupees)
    APPLY_OPTIONAL(swordHealth)
    APPLY_OPTIONAL(isMagicAcquired)
    APPLY_OPTIONAL(isDoubleMagicAcquired)
    APPLY_OPTIONAL(isDoubleDefenseAcquired)
    APPLY_OPTIONAL(bgsFlag)
    APPLY_OPTIONAL(savedSceneNum)
    APPLY_OPTIONAL(worldMapAreaData)
#undef APPLY_OPTIONAL

    if (persistent.childEquips.has_value()) {
        gSaveContext.childEquips = *persistent.childEquips;
    }
    if (persistent.adultEquips.has_value()) {
        gSaveContext.adultEquips = *persistent.adultEquips;
    }
    if (persistent.equips.has_value()) {
        gSaveContext.equips = *persistent.equips;
    }
    if (persistent.inventory.has_value()) {
        gSaveContext.inventory = *persistent.inventory;
    }
    if (persistent.sceneFlags.has_value()) {
        for (size_t i = 0; i < SceneFlagCount; ++i) {
            gSaveContext.sceneFlags[i] = persistent.sceneFlags->at(i);
        }
    }
    if (persistent.gsFlags.has_value()) {
        for (size_t i = 0; i < persistent.gsFlags->size(); ++i) {
            gSaveContext.gsFlags[i] = static_cast<s32>(persistent.gsFlags->at(i));
        }
    }
    if (persistent.eventChkInf.has_value()) {
        for (size_t i = 0; i < persistent.eventChkInf->size(); ++i) {
            gSaveContext.eventChkInf[i] = persistent.eventChkInf->at(i);
        }
    }
    if (persistent.itemGetInf.has_value()) {
        for (size_t i = 0; i < persistent.itemGetInf->size(); ++i) {
            gSaveContext.itemGetInf[i] = persistent.itemGetInf->at(i);
        }
    }
    if (persistent.infTable.has_value()) {
        for (size_t i = 0; i < persistent.infTable->size(); ++i) {
            gSaveContext.infTable[i] = persistent.infTable->at(i);
        }
    }

    if (persistent.entranceIndex.has_value()) {
        pendingRuntimeState = state.contains("runtime") ? state.at("runtime") : json::object();
        gPlayState->nextEntranceIndex = static_cast<s16>(*persistent.entranceIndex);
        gPlayState->transitionTrigger = TRANS_TRIGGER_START;
        gPlayState->transitionType = TRANS_TYPE_INSTANT;
        gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
        waitingForTransition = true;
        return true;
    }
    if (state.contains("runtime")) {
        return ApplyRuntimeState(state.at("runtime"), error);
    }
    if (persistent.sceneFlags.has_value() && gPlayState->sceneNum >= 0 &&
        gPlayState->sceneNum < static_cast<s16>(SceneFlagCount)) {
        const auto& flags = gSaveContext.sceneFlags[gPlayState->sceneNum];
        gPlayState->actorCtx.flags.chest = flags.chest;
        gPlayState->actorCtx.flags.swch = flags.swch;
        gPlayState->actorCtx.flags.clear = flags.clear;
        gPlayState->actorCtx.flags.collect = flags.collect;
    }
    return true;
}

void WiiUTestControl::FinishPendingTransition() {
    transitionCompleted = false;
    if (!pendingTransitionRequestId.has_value()) {
        return;
    }
    std::string error;
    bool success = true;
    if (pendingRuntimeState.has_value() && !pendingRuntimeState->empty()) {
        success = ApplyRuntimeState(*pendingRuntimeState, error);
    }

    json response = {
        { "schema", ProtocolVersion },
        { "type", "event" },
        { "event", "state_applied" },
        { "id", *pendingTransitionRequestId },
        { "status", success ? "success" : "failure" },
    };
    if (success) {
        response["state"] = CaptureState();
        OSReport("[SoH][test-control] state applied after transition\n");
    } else {
        response["error"] = error;
        OSReport("[SoH][test-control] post-transition apply failed: %s\n", error.c_str());
    }
    if (pendingTransitionClientConnected) {
        SendJson(response);
        CloseClient();
    }
    pendingTransitionRequestId.reset();
    pendingRuntimeState.reset();
    pendingTransitionClientConnected = false;
}

void WiiUTestControl::HandlePacket(const std::string& packet) {
    json request;
    json id = nullptr;
    try {
        request = json::parse(packet);
        if (request.contains("id")) {
            id = request.at("id");
        }
        if (!request.is_object()) {
            throw std::runtime_error("request must be a JSON object");
        }
        if (request.value("schema", ProtocolVersion) != ProtocolVersion) {
            throw std::runtime_error("unsupported schema version");
        }
        if (!request.contains("operation") || !request.at("operation").is_string()) {
            throw std::runtime_error("request must contain a string operation");
        }
        const std::string operation = request.at("operation").get<std::string>();

        if (operation == "ping") {
            SendJson({ { "schema", ProtocolVersion },
                       { "type", "result" },
                       { "id", id },
                       { "operation", operation },
                       { "status", "success" },
                       { "version", std::string(gBuildVersion) },
                       { "commit", std::string(gGitCommitHash) } });
            CloseClient();
            return;
        }
        if (operation == "get_state") {
            SendJson({ { "schema", ProtocolVersion },
                       { "type", "result" },
                       { "id", id },
                       { "operation", operation },
                       { "status", "success" },
                       { "state", CaptureState() } });
            OSReport("[SoH][test-control] state captured\n");
            CloseClient();
            return;
        }
        if (operation == "apply_state") {
            if (!request.contains("state")) {
                throw std::runtime_error("apply_state requires a state object");
            }
            std::string error;
            bool waitingForTransition = false;
            if (!ApplyState(request.at("state"), error, waitingForTransition)) {
                throw std::runtime_error(error);
            }
            if (waitingForTransition) {
                pendingTransitionRequestId = id;
                pendingTransitionClientConnected = true;
                SendJson({ { "schema", ProtocolVersion },
                           { "type", "result" },
                           { "id", id },
                           { "operation", operation },
                           { "status", "transitioning" } });
                OSReport("[SoH][test-control] state accepted; waiting for transition\n");
            } else {
                SendJson({ { "schema", ProtocolVersion },
                           { "type", "result" },
                           { "id", id },
                           { "operation", operation },
                           { "status", "success" },
                           { "state", CaptureState() } });
                OSReport("[SoH][test-control] state applied\n");
                CloseClient();
            }
            return;
        }
        if (operation == "checkpoint" || operation == "restore") {
            if (!GameInteractor::IsSaveLoaded(true)) {
                throw std::runtime_error("no game is loaded");
            }
            if (pendingTransitionRequestId.has_value() || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
                throw std::runtime_error("wait for the current scene transition to finish");
            }
            int64_t slot = 0;
            std::string error;
            if (!request.contains("slot") || !ReadIntegerValue(request.at("slot"), 0, 2, slot, error, "slot")) {
                throw std::runtime_error(error.empty() ? "checkpoint operations require slot 0, 1, or 2" : error);
            }
            const RequestType type = operation == "checkpoint" ? RequestType::SAVE : RequestType::LOAD;
            const SaveStateReturn result =
                OTRGlobals::Instance->gSaveStateMgr->AddRequest({ static_cast<unsigned int>(slot), type });
            const bool success = result == SaveStateReturn::SUCCESS;
            SendJson({ { "schema", ProtocolVersion },
                       { "type", "result" },
                       { "id", id },
                       { "operation", operation },
                       { "slot", slot },
                       { "status", success ? "success" : "failure" },
                       { "result", SaveStateResultName(result) } });
            OSReport("[SoH][test-control] %s slot=%lld result=%s\n", operation.c_str(), static_cast<long long>(slot),
                     SaveStateResultName(result));
            CloseClient();
            return;
        }
        throw std::runtime_error("unknown operation: " + operation);
    } catch (const std::exception& exception) {
        SendJson({ { "schema", ProtocolVersion },
                   { "type", "result" },
                   { "id", id },
                   { "status", "failure" },
                   { "error", exception.what() } });
        OSReport("[SoH][test-control] request failed: %s\n", exception.what());
        CloseClient();
    }
}

#endif
