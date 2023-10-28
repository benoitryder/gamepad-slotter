#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <XInput.h>
#include <ViGEm/Client.h>

namespace ranges = std::ranges;
using namespace std::chrono_literals;


/// Wrap ViGEmClient
struct VigemClient {
  using Pad = PVIGEM_TARGET;

  VigemClient() {
    auto const client = vigem_alloc();
    if (!client) {
      throw std::runtime_error("vigem_alloc() failed");
    }

    auto const retval = vigem_connect(client);
    if (!VIGEM_SUCCESS(retval)) {
      throw std::runtime_error(std::format("ViGEm Bus connection failed with error code: 0x{:x}", static_cast<int>(retval)));
    }

    m_client = client;
  }

  /// Register a virtual gamepad, return a handle to be used by other methods
  Pad addPad() {
    auto const pad = vigem_target_x360_alloc();
    if (!pad) {
      throw std::runtime_error("vigem_target_x360_alloc() failed");
    }

    auto const retval = vigem_target_add(m_client, pad);
    if (!VIGEM_SUCCESS(retval)) {
      throw std::runtime_error(std::format("Target plugin failed with error code: 0x{:x}", static_cast<int>(retval)));
    }

    m_pads.push_back(pad);
    return pad;
  }

  /// Remove a gamepad added with `addPad()`
  void removePad(Pad pad) {
    auto it = ranges::find(m_pads, pad);
    if (it == m_pads.end()) {
      throw std::runtime_error("removePad(): invalid pad");
    }

    vigem_target_remove(m_client, pad);
    vigem_target_free(pad);
    m_pads.erase(it);
  }

  /// Get the slot index of a gamepad
  size_t getPadIndex(Pad pad) const {
    ULONG index = -1;
    auto retval = vigem_target_x360_get_user_index(m_client, pad, &index);
    if (!VIGEM_SUCCESS(retval)) {
      throw std::runtime_error(std::format("vigem_target_x360_get_user_index() failed: 0x{:x}", static_cast<int>(retval)));
    }
    return index;
  }

  ~VigemClient() {
    for (auto const& pad : m_pads) {
      vigem_target_remove(m_client, pad);
      vigem_target_free(pad);
    }
    vigem_disconnect(m_client);
    vigem_free(m_client);
  }

  PVIGEM_CLIENT m_client;
  std::vector<PVIGEM_TARGET> m_pads;
};

/// Manage state of connected pads
struct ConnectedPads {
  /// State of a gamepad slot
  ///
  /// Some states are invalid/erroneous
  struct Slot {
    bool m_plugged = false;
    VigemClient::Pad m_managed = nullptr;
  };

  ConnectedPads() {
    // Initiliaze the state, don't log alreay connected pads
    for (size_t i = 0; i < m_slots.size(); ++i) {
      m_slots[i].m_plugged = isPadPlugged(i);
    }
  }

  /// Return true if given slot is plugged
  bool isPlugged(size_t index) const {
    if (index >= m_slots.size()) {
      std::cerr << std::format("ERROR: invalid slot: {}\n", index);
    }
    return m_slots.at(index).m_plugged;
  }

  /// Print the current state
  void printState() const {
    std::cout << "State:";
    for (auto const& slot : m_slots) {
      char state = '?';
      if (slot.m_plugged && slot.m_managed) {
        state = 'x';
      } else if (slot.m_plugged && !slot.m_managed) {
        state = 'O';
      } else if (!slot.m_plugged && slot.m_managed) {
        state = 'X';  // erroneous
      } else if (!slot.m_plugged && !slot.m_managed) {
        state = '-';
      }
      std::cout << std::format("  {}", state);
    }
    std::cout << "\n";
  }

  /// Update plugged pads using `XInputGetState`
  ///
  /// Return `true` if state changed.
  bool updatePlugged() {
    bool changed = false;
    for (size_t i = 0; i < XUSER_MAX_COUNT; ++i) {
      bool plugged = isPadPlugged(i);
      auto& slot = m_slots[i];
      // Log state changes and invalid states
      if (slot.m_managed) {
        if (!plugged) {
          std::cerr << std::format("WARNING: virtual pad {} unplugged\n", i);
        }
      } else if (slot.m_plugged != plugged) {
        std::cout << std::format("Pad {} {}\n", i, plugged ? "plugged" : "unplugged");
      }

      changed |= slot.m_plugged != plugged;
      slot.m_plugged = plugged;
    }
    return changed;
  }

  /// Fill all unplugged slots with managed pads
  void fillAll() {
    // Count free slots (i.e. how many pads to add)
    int free = 0;
    for (auto const& slot : m_slots) {
      if (!slot.m_plugged) {
        ++free;
      }
    }

    // Create pads for the unplugged slots
    for (int i = 0; i < free; ++i) {
      auto pad = m_client.addPad();
      auto index = m_client.getPadIndex(pad);
      auto& slot = m_slots.at(index);
      if (slot.m_managed) {
        std::cerr << std::format("WARNING: virtual pad created on an already managed slot: {}\n", index);
        m_client.removePad(pad);
      } else if (slot.m_plugged) {
        std::cerr << std::format("WARNING: virtual pad created on an already plugged slot: {}\n", index);
        m_client.removePad(pad);
      } else {
        slot.m_plugged = true;
        slot.m_managed = pad;
      }
    }

    // Check final state
    updatePlugged();  // will log unplugged managed pads
    for (size_t i = 0; i < m_slots.size(); ++i) {
      if (!m_slots[i].m_plugged) {
        std::cerr << std::format("WARNING: slot {} still unplugged\n", i);
      }
    }
  }

  /// Free the given slot, if it is managed
  void freeSlot(size_t index) {
    if (index >= m_slots.size()) {
      std::cerr << std::format("ERROR: invalid slot: {}\n", index);
    }
    auto& slot = m_slots.at(index);
    if (!slot.m_managed) {
      std::cerr << std::format("ERROR: cannot free unmanaged slot: {}\n", index);
      return;
    }

    m_client.removePad(slot.m_managed);
    slot.m_managed = nullptr;
    slot.m_plugged = isPadPlugged(index);
    if (!slot.m_plugged) {
      std::cerr << std::format("WARNING: managed slot {} has been freed but is still plugged\n", index);
    }
  }

  /// Get state of a single slot
  static bool isPadPlugged(size_t index) {
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));
    return XInputGetState(index, &state) == ERROR_SUCCESS;
  }

  VigemClient m_client;
  std::array<Slot, XUSER_MAX_COUNT> m_slots;
};


int main() {
  ConnectedPads pads;
  pads.printState();

  size_t const target = 0;

  if (pads.isPlugged(target)) {
    std::cout << std::format("Pad {} already plugged\n", target);
    return 0;
  }

  std::cout << std::format("Blocking all slots except {}\n", target);
  pads.fillAll();

  std::cout << std::format("Waiting pad on slot {}...\n", target);
  pads.printState();
  for (;;) {
    std::this_thread::sleep_for(100ms);
    if (pads.updatePlugged()) {
      pads.printState();
      pads.fillAll();
      if (pads.isPlugged(target)) {
        break;
      }
    }
  }
}

