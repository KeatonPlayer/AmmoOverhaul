#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace dc {
// No actor pointers are dereferenced by this container. A pointer and a FormID
// together distinguish actor instances. State is transient, never serialized.
struct Key {
    std::uintptr_t actor{};
    std::uint32_t actorID{}, weaponID{};
    bool operator==(const Key& r) const {
        return actor == r.actor && actorID == r.actorID && weaponID == r.weaponID;
    }
};
struct Shot { std::uint32_t ammoID{}; int rule{-1}; std::uint64_t time{}; };
class ShotQueue {
    struct Slot {
        Key key{};
        std::array<Shot, 32> shots{};
        std::size_t head{}, count{};
        std::uint64_t touched{};
        bool used{}, blocked{};
    };
    std::array<Slot, 128> slots_{};
public:
    std::uint64_t lifetime = 30000;
    void clear() { slots_ = {}; }
    // Returns false if overflow makes the event pairing ambiguous. That slot
    // stays in fallback mode until an inactivity interval or a lifecycle reset.
    bool push(Key key, Shot shot) {
        Slot* s = nullptr;
        for (auto& x : slots_) if (x.used && x.key == key) { s = &x; break; }
        if (!s) {
            for (auto& x : slots_) if (!x.used || shot.time - x.touched > lifetime) { s = &x; break; }
            if (!s) return false;
            *s = {}; s->used = true; s->key = key;
        }
        if (shot.time - s->touched > lifetime) { s->head = s->count = 0; s->blocked = false; }
        s->touched = shot.time;
        if (s->blocked) return false;
        if (s->count == s->shots.size()) {
            s->count = 0; s->blocked = true; return false;
        }
        s->shots[(s->head + s->count++) % s->shots.size()] = shot;
        return true;
    }
    bool pop(Key key, std::uint64_t now, Shot& out) {
        for (auto& s : slots_) if (s.used && s.key == key) {
            if (s.blocked) return false;
            while (s.count) {
                out = s.shots[s.head]; s.head = (s.head + 1) % s.shots.size(); --s.count;
                if (now - out.time <= lifetime) return true;
            }
            return false;
        }
        return false;
    }
};
}
