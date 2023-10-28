#include <array>
#include <chrono>
#include <cstring>
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


// Code assume slot indexes are 1-character wide
static_assert(XUSER_MAX_COUNT + 1 <= 9);

/// Wrap ViGEmClient
struct VigemClient {
  using Pad = PVIGEM_TARGET;

  VigemClient() {
    auto const client = vigem_alloc();
    if (!client) {
      throw std::runtime_error("vigem_alloc() failed");
    }

    checkSuccess(vigem_connect(client), "vigem_connect() failed");
    m_client = client;
  }

  /// Register a virtual gamepad, return a handle to be used by other methods
  Pad addPad() {
    auto const pad = vigem_target_x360_alloc();
    if (!pad) {
      throw std::runtime_error("vigem_target_x360_alloc() failed");
    }

    checkSuccess(vigem_target_add(m_client, pad), "vigem_target_add() failed");

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

  ~VigemClient() {
    for (auto const& pad : m_pads) {
      vigem_target_remove(m_client, pad);
      vigem_target_free(pad);
    }
    vigem_disconnect(m_client);
    vigem_free(m_client);
  }


  /// Check a ViGEmClient, throw on error
  static void checkSuccess(VIGEM_ERROR retval, char const* message) {
    if (!VIGEM_SUCCESS(retval)) {
      throw std::runtime_error(std::format("{}: 0x{:X}", message, static_cast<unsigned int>(retval)));
    }
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
      std::cerr << std::format("ERROR: invalid slot: {}\n", index + 1);
    }
    return m_slots.at(index).m_plugged;
  }

  /// Print the current state
  void printState() const {
    std::cout << "State:";
    for (size_t i = 0; i < m_slots.size(); ++i) {
      auto const& slot = m_slots[i];
      char state = '?';
      if (slot.m_plugged && slot.m_managed) {
        state = 'x';
      } else if (slot.m_plugged && !slot.m_managed) {
        state = '1' + i;
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
    for (size_t i = 0; i < m_slots.size(); ++i) {
      bool plugged = isPadPlugged(i);
      auto& slot = m_slots[i];
      // Log state changes and invalid states
      if (slot.m_managed) {
        if (!plugged) {
          std::cerr << std::format("WARNING: virtual pad unplugged on slot {}\n", i + 1);
        }
      } else if (slot.m_plugged != plugged) {
        std::cout << std::format("Pad {} {}\n", i + 1, plugged ? "plugged" : "unplugged");
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

    // `vigem_target_x360_get_user_index()` is unreliable; it sometimes fails.
    // Assume no new device is manually plugged in between and poll `XInputGetState()`
    auto const pollNewIndex = [&]() -> size_t {
      int constexpr timeout_tries = 100;
      auto constexpr timeout_delay = 1000ms / timeout_tries;

      for (int tries = 0; tries < timeout_tries; ++tries) {
        for (size_t i = 0; i < m_slots.size(); ++i) {
          if (m_slots[i].m_plugged) {
            continue;  // don't poll already plugged slots
          }
          if (isPadPlugged(i)) {
            return i;
          }
        }
        std::this_thread::sleep_for(timeout_delay);
      }
      throw std::runtime_error("failed to get index of new virtual pad (timeout)");
    };

    // Create pads for the unplugged slots
    for (int i = 0; i < free; ++i) {
      auto pad = m_client.addPad();
      size_t const index = pollNewIndex();
      auto& slot = m_slots.at(index);
      if (slot.m_managed) {
        std::cerr << std::format("WARNING: virtual pad created on an already managed slot: {}\n", index + 1);
        m_client.removePad(pad);
      } else if (slot.m_plugged) {
        std::cerr << std::format("WARNING: virtual pad created on an already plugged slot: {}\n", index + 1);
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
        std::cerr << std::format("WARNING: slot {} still unplugged\n", i + 1);
      }
    }
  }

  /// Free the given slot, if it is managed
  void freeSlot(size_t index) {
    if (index >= m_slots.size()) {
      std::cerr << std::format("ERROR: invalid slot: {}\n", index + 1);
    }
    auto& slot = m_slots.at(index);
    if (!slot.m_managed) {
      std::cerr << std::format("ERROR: cannot free unmanaged slot: {}\n", index + 1);
      return;
    }

    m_client.removePad(slot.m_managed);
    slot.m_managed = nullptr;

    // Wait for pad to be actually unplugged
    int constexpr timeout_tries = 100;
    auto constexpr timeout_delay = 1000ms / timeout_tries;
    for (int tries = 0; tries < timeout_tries; ++tries) {
      slot.m_plugged = isPadPlugged(index);
      if (!slot.m_plugged) {
        break;
      }
      std::this_thread::sleep_for(timeout_delay);
    }
    if (slot.m_plugged) {
      std::cerr << std::format("WARNING: managed slot {} has been freed but is still plugged\n", index + 1);
    }
  }

  /// Fill all slots except the given one
  ///
  /// Do nothing if state is already fine.
  void fillAllButOne(size_t index) {
    for (size_t i = 0; i < m_slots.size(); ++i) {
      if (i != index && !m_slots[i].m_plugged) {
        fillAll();
        freeSlot(index);
      }
    }
    // Nothing to do
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


int main(int argc, char* argv[]) {
  size_t target = SIZE_MAX;  // invalid, checked to display usage
  if (argc < 2) {
    target = 0;  // default: wait for first slot
  } else if (argc == 2) {
    // Parse a 1-character index
    if (std::strlen(argv[1]) == 1) {
      char c = argv[1][0];
      if (c >= '1' && c < '1' + XUSER_MAX_COUNT) {
        target = c - '1';
      }
    }
  }
  if (target == SIZE_MAX) {
    std::cerr << std::format("usage: {} [1-{}]\n", argv[0], XUSER_MAX_COUNT);
    return EXIT_FAILURE;
  }

  try {
    ConnectedPads pads;
    pads.printState();

    if (pads.isPlugged(target)) {
      std::cout << std::format("Pad {} already plugged\n", target + 1);
      return EXIT_SUCCESS;
    }

    pads.fillAllButOne(target);
    std::cout << std::format("Waiting pad on slot {}...\n", target + 1);
    pads.printState();
    for (;;) {
      std::this_thread::sleep_for(100ms);
      if (pads.updatePlugged()) {
        if (pads.isPlugged(target)) {
          break;
        }
        // Fill again, in case an unmanaged gamepad has been unplugged
        pads.fillAllButOne(target);
        pads.printState();
      }
    }
  } catch (std::exception const& e) {
    std::cerr << "FATAL: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}

