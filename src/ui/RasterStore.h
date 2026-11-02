#pragma once

#include <QCache>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QtGlobal>

// Identifies one cached raster: a whole page (tileX == tileY == -1) or a tile of it.
struct RasterKey
{
    int page = -1;
    int width = 0;
    int height = 0;
    int tileX = -1;
    int tileY = -1;
    bool annotated = true;

    bool isTile() const noexcept { return tileX >= 0; }

    friend bool operator==(const RasterKey &a, const RasterKey &b) noexcept
    {
        return a.page == b.page && a.width == b.width && a.height == b.height
               && a.tileX == b.tileX && a.tileY == b.tileY && a.annotated == b.annotated;
    }
    friend size_t qHash(const RasterKey &key, size_t seed = 0) noexcept
    {
        return qHashMulti(seed, key.page, key.width, key.height, key.tileX, key.tileY,
                          key.annotated);
    }
};

// A pixmap cache plus the render state of each key: failed (do not retry),
// stale (shown until a fresh render arrives) and pending (render in flight).
struct RasterStore
{
    explicit RasterStore(int maxCostKilobytes)
        : cache(maxCostKilobytes)
    {
    }

    QCache<RasterKey, QPixmap> cache;
    QSet<RasterKey> failed;
    QSet<RasterKey> stale;
    QSet<RasterKey> pending;

    void clear()
    {
        cache.clear();
        failed.clear();
        stale.clear();
    }

    void markAllStale()
    {
        for (const RasterKey &key : cache.keys())
            stale.insert(key);
    }

    void markPagesStale(const QList<int> &pages)
    {
        for (const RasterKey &key : cache.keys()) {
            if (pages.contains(key.page))
                stale.insert(key);
        }
        for (auto it = failed.begin(); it != failed.end();) {
            if (pages.contains(it->page))
                it = failed.erase(it);
            else
                ++it;
        }
    }

    // Drops every matching entry and its state; returns how many pixmaps were cached.
    template <typename Predicate>
    int removeIf(Predicate predicate)
    {
        int removed = 0;
        for (const RasterKey &key : cache.keys()) {
            if (predicate(key)) {
                cache.remove(key);
                ++removed;
            }
        }
        for (auto it = stale.begin(); it != stale.end();) {
            if (predicate(*it))
                it = stale.erase(it);
            else
                ++it;
        }
        for (auto it = failed.begin(); it != failed.end();) {
            if (predicate(*it))
                it = failed.erase(it);
            else
                ++it;
        }
        return removed;
    }
};
