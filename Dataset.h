#pragma once
// The training pictures: kept in memory (128 x 128, 8-bit RGB) and saved to the Xbox's storage as they arrive, so they are
// still there the next time the app starts. Thread-safe: the phone page, the worker thread and the TV screen all use it.

#include "ImageUtil.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace XboxOTFM {

struct DatasetCardInfo {
    uint64_t id = 0;
    uint64_t hash = 0; // fingerprint of the pixels (the web page uses it so a cached thumbnail can never show the wrong picture)
    uint32_t w = 0;
    uint32_t h = 0;
    std::string name;
};

struct AugmentOptions {
    bool flip = false;  // mirror left/right half of the time
    bool zoom = false;  // random zoom 85-100% with a random shift
    bool color = false; // small random brightness / contrast changes
};

class Dataset {
public:
    static constexpr uint32_t kCardSize = 128;

    // folder: where cards.otds is kept (created if needed). An empty folder = memory only (used by tests).
    explicit Dataset(std::string folder = std::string(), size_t maxCards = 3000);
    ~Dataset();
    Dataset(const Dataset&) = delete;
    Dataset& operator=(const Dataset&) = delete;

    // Loads the saved pictures. Problems that can be survived (a damaged tail, an unwritable folder) are reported in warnings.
    bool Open(std::vector<std::string>* warnings = nullptr);

    enum class AddStatus { Added, Duplicate, Full, Invalid, DiskError };
    struct AddResult {
        AddStatus status = AddStatus::Invalid;
        uint64_t id = 0;
        std::string message;
    };
    // rgb: w*h*3 bytes. Anything that is not 128x128 is centre-cropped and resized.
    AddResult Add(const uint8_t* rgb, uint32_t w, uint32_t h, const std::string& name);

    bool Remove(uint64_t id);
    size_t RemoveMany(const std::vector<uint64_t>& ids);
    void Clear();

    size_t Size() const;
    size_t Capacity() const { return m_maxCards; }
    uint64_t Version() const;
    uint64_t DiskBytes() const;
    bool Persistent() const { return !m_folder.empty(); }

    std::vector<DatasetCardInfo> List(size_t offset, size_t limit) const;
    bool GetImage(uint64_t id, Rgb8Image& out, std::string* name = nullptr) const;

    // Fills outCHW with `count` pictures as [count, 3, res, res] floats in [-1, 1]. Pictures are dealt in shuffled passes over
    // the whole set (every picture once per pass). Returns false if there are no pictures.
    bool SampleBatch(uint32_t count, uint32_t res, const AugmentOptions& aug, std::mt19937_64& rng, std::vector<float>& outCHW);
    // How many full passes (epochs) have been dealt so far, and how far into the current pass we are (0..1).
    double EpochProgress() const;
    void ResetEpochCounter();

private:
    struct Card {
        uint64_t id = 0;
        uint64_t hash = 0;
        std::string name;
        std::vector<uint8_t> rgb; // 128 * 128 * 3
    };

    bool OpenLocked(std::vector<std::string>* warnings);
    bool WriteHeader(FILE* f) const;
    bool AppendAddRecord(const Card& c);
    bool AppendRemoveRecord(uint64_t id);
    bool RewriteLocked();                  // writes only the live cards into a fresh file
    void MaybeCompactLocked();
    void CloseFileLocked();
    bool ReopenForAppendLocked();
    void IndexCardLocked(const Card& c);
    void RebuildIndexLocked();

    std::string FilePath() const;
    std::string TempFilePath() const;

    const std::string m_folder;
    const size_t m_maxCards;

    mutable std::mutex m_mutex;
    std::vector<Card> m_cards;
    std::unordered_map<uint64_t, uint64_t> m_hashToId;
    uint64_t m_nextId = 1;
    uint64_t m_version = 1;
    FILE* m_file = nullptr;
    uint64_t m_fileBytes = 0;
    uint64_t m_fileRecords = 0;   // records in the file (adds + removes)
    uint64_t m_deadRecords = 0;   // records that no longer describe a live card
    bool m_diskBroken = false;

    std::vector<uint32_t> m_order; // shuffled indices for the current pass
    size_t m_cursor = 0;
    bool m_orderDirty = true;
    uint64_t m_epochs = 0;
};

} // namespace XboxOTFM
