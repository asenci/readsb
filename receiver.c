#include "readsb.h"

#define RECEIVER_MAX_RANGE 800e3

uint32_t receiverHash(uint64_t id) {
    uint64_t h = 0x30732349f7810465ULL ^ (4 * 0x2127599bf4325c37ULL);
    h ^= mix_fasthash(id);

    h -= (h >> 32);
    h &= (1ULL << 32) - 1;
    h -= (h >> RECEIVER_TABLE_HASH_BITS);

    return h & (RECEIVER_TABLE_SIZE - 1);
}

struct receiver *receiverGet(uint64_t id) {
    struct receiver *r = Modes.receiverTable[receiverHash(id)];

    while (r && r->id != id) {
        r = r->next;
    }
    return r;
}
struct receiver *receiverCreate(uint64_t id) {
    struct receiver *r = receiverGet(id);
    if (r)
        return r;
    if (Modes.receiverCount > 4 * RECEIVER_TABLE_SIZE)
        return NULL;
    uint32_t hash = receiverHash(id);
    r = aligned_malloc(sizeof(struct receiver));
    *r = (struct receiver) {0};
    r->id = id;
    r->next = Modes.receiverTable[hash];
    r->firstSeen = r->lastSeen = mstime();
    Modes.receiverTable[hash] = r;
    Modes.receiverCount++;
    if (Modes.receiverCount % (RECEIVER_TABLE_SIZE / 8) == 0)
        fprintf(stderr, "receiverTable fill: %0.8f\n", Modes.receiverCount / (double) RECEIVER_TABLE_SIZE);
    if (Modes.debug_receiver && Modes.receiverCount % 128 == 0)
        fprintf(stderr, "receiverCount: %"PRIu64"\n", Modes.receiverCount);
    return r;
}
void receiverTimeout(int part, int nParts, int64_t now) {
    int stride = RECEIVER_TABLE_SIZE / nParts;
    int start = stride * part;
    int end = start + stride;
    //fprintf(stderr, "START: %8d END: %8d\n", start, end);
    for (int i = start; i < end; i++) {
        struct receiver **r = &Modes.receiverTable[i];
        struct receiver *del;
        while (*r) {
            /*
            receiver *b = *r;
            fprintf(stderr, "%016"PRIx64" %9"PRu64" %4.0f %4.0f %4.0f %4.0f\n",
                    b->id, b->positionCounter,
                    b->latMin, b->latMax, b->lonMin, b->lonMax);
            */
            if (
                    (Modes.receiverCount > RECEIVER_TABLE_SIZE && (*r)->lastSeen < now - 20 * MINUTES)
                    || (now > (*r)->lastSeen + 24 * HOURS)
                    || ((*r)->badExtent && now > (*r)->badExtent + 30 * MINUTES)
               ) {

                del = *r;
                *r = (*r)->next;
                Modes.receiverCount--;
                free(del);
            } else {
                r = &(*r)->next;
            }
        }
    }
}
void receiverCleanup() {
    for (int i = 0; i < RECEIVER_TABLE_SIZE; i++) {
        struct receiver *r = Modes.receiverTable[i];
        struct receiver *next;
        while (r) {
            next = r->next;
            free(r);
            r = next;
        }
    }
}
int receiverPositionReceived(struct aircraft *a, struct modesMessage *mm, double lat, double lon, int64_t now) {
    if (lat > 85.0 || lat < -85.0 || lon < -175 || lon > 175)
        return -1;
    int reliabilityRequired = Modes.position_persistence * 3 / 4;
    if (Modes.viewadsb || Modes.receiver_focus) {
        reliabilityRequired = imin(2, Modes.position_persistence);
    }
    if (
            ! (
                mm->source == SOURCE_ADSB && mm->cpr_type != CPR_SURFACE
                && a->pos_reliable_odd >= reliabilityRequired
                && a->pos_reliable_even >= reliabilityRequired
              )
       ) {
        return -1;
    }
    double distance = 0;
    uint64_t id = mm->receiverId;
    struct receiver *r = receiverGet(id);

    if (!r || r->positionCounter == 0) {
        r = receiverCreate(id);
        if (!r)
            return -1;
        r->lonMin = lon;
        r->lonMax = lon;
        r->latMin = lat;
        r->latMax = lat;
    } else {

        // diff before applying new position
        struct receiver before = *r;
        double latDiff = before.latMax - before.latMin;
        double lonDiff = before.lonMax - before.lonMin;

        double rlat = r->latMin + latDiff / 2;
        double rlon = r->lonMin + lonDiff / 2;

        distance = greatcircle(rlat, rlon, lat, lon, 1);

        if (distance < RECEIVER_MAX_RANGE) {
            r->lonMin = fmin(r->lonMin, lon);
            r->latMin = fmin(r->latMin, lat);

            r->lonMax = fmax(r->lonMax, lon);
            r->latMax = fmax(r->latMax, lat);
            r->goodCounter++;
            r->badCounter = fmax(0, r->badCounter - 0.5);
        }

        if (!r->badExtent && distance > RECEIVER_MAX_RANGE) {
            int badExtent = 1;
            for (int i = 0; i < RECEIVER_BAD_AIRCRAFT; i++) {
                struct bad_ac *bad = &r->badAircraft[i];
                if (bad->addr == a->addr) {
                    badExtent = 0;
                    break;
                }
            }
            for (int i = 0; i < RECEIVER_BAD_AIRCRAFT; i++) {
                struct bad_ac *bad = &r->badAircraft[i];
                if (now - bad->ts > 3 * MINUTES) {
                    // new entry
                    bad->ts = now;
                    bad->addr = a->addr;
                    badExtent = 0;
                    break;
                }
            }
            if (badExtent) {
                r->badExtent = now;

                if (Modes.debug_receiver) {
                    char uuid[32]; // needs 18 chars and null byte
                    sprint_uuid1(r->id, uuid);
                    fprintf(stderr, "receiverBadExtent: %0.0f nmi hex: %06x id: %s #pos: %9"PRIu64" %12.5f %12.5f %4.0f %4.0f %4.0f %4.0f\n",
                            distance / 1852.0, a->addr, uuid, r->positionCounter,
                            lat, lon,
                            before.latMin, before.latMax,
                            before.lonMin, before.lonMax);
                }
            }
        }
    }

    r->positionCounter++;
    r->lastSeen = now;

    if (distance > RECEIVER_MAX_RANGE) {
        return -2;
    }

    return 1;
}

struct receiver *receiverGetReference(uint64_t id, double *lat, double *lon, struct aircraft *a, int noDebug) {
    struct receiver *r = receiverGet(id);
    if (!(Modes.debug_receiver && a && a->addr == Modes.cpr_focus)) {
        noDebug = 1;
    }
    if (!r) {
        if (!noDebug) {
            fprintf(stderr, "id:%016"PRIx64" NOREF: receiverId not known\n", id);
        }
        return NULL;
    }


    double latDiff = r->latMax - r->latMin;
    double lonDiff = r->lonMax - r->lonMin;

    *lat = r->latMin + latDiff / 2;
    *lon = r->lonMin + lonDiff / 2;

    uint32_t positionCounterRequired = (Modes.viewadsb || Modes.receiver_focus) ? 4 : 100;
    if (r->positionCounter < positionCounterRequired || r->badExtent) {
        if (!noDebug) {
            fprintf(stderr, "id:%016"PRIx64" NOREF: #posCounter:%9"PRIu64" refLoc: %4.0f,%4.0f lat: %4.0f to %4.0f lon: %4.0f to %4.0f\n",
                    r->id, r->positionCounter,
                    *lat, *lon,
                    r->latMin, r->latMax,
                    r->lonMin, r->lonMax);
        }
        return NULL;
    }

    if (!noDebug) {
        fprintf(stderr, "id:%016"PRIx64" #posCounter:%9"PRIu64" refLoc: %4.0f,%4.0f lat: %4.0f to %4.0f lon: %4.0f to %4.0f\n",
                r->id, r->positionCounter,
                *lat, *lon,
                r->latMin, r->latMax,
                r->lonMin, r->lonMax);
    }

    return r;
}
void receiverTest() {
    int64_t now = mstime();
    for (uint64_t i = 0; i < (1<<22); i++) {
        uint64_t id = i << 22;
        receiver *r = receiverGet(id);
        if (!r)
            r = receiverCreate(id);
        if (r)
            r->lastSeen = now;
    }
    printf("%"PRIu64"\n", Modes.receiverCount);
    for (int i = 0; i < (1<<22); i++) {
        receiver *r = receiverGet(i);
        if (!r)
            r = receiverCreate(i);
    }
    printf("%"PRIu64"\n", Modes.receiverCount);
    receiverTimeout(0, 1, mstime());
    printf("%"PRIu64"\n", Modes.receiverCount);
}

int receiverCheckBad(uint64_t id, int64_t now) {
    struct receiver *r = receiverGet(id);
    if (r && now < r->timedOutUntil)
        return 1;
    else
        return 0;
}

struct receiver *receiverBad(uint64_t id, uint32_t addr, int64_t now) {
    struct receiver *r = receiverGet(id);

    if (!r)
        r = receiverCreate(id);

    int64_t timeout = 12 * SECONDS;

    if (r && now + (timeout * 2 / 3) > r->timedOutUntil) {
        r->lastSeen = now;
        r->badCounter++;
        if (r->badCounter > 5.99) {
            r->timedOutCounter++;
            if (Modes.debug_garbage) {
                fprintf(stderr, "timeout receiverId: %016"PRIx64" hex: %06x #good: %6d #bad: %5.0f #timeouts: %u\n",
                        r->id, addr, r->goodCounter, r->badCounter, r->timedOutCounter);
            }
            r->timedOutUntil = now + timeout;
            r->goodCounter = 0;
            r->badCounter = 0;
        }
        return r;
    } else {
        return NULL;
    }
}

struct char_buffer generateReceiversJson() {
    struct char_buffer cb;
    int64_t now = mstime();

    size_t buflen = 1*1024*1024; // The initial buffer is resized as needed
    char *buf = (char *) aligned_malloc(buflen), *p = buf, *end = buf + buflen;

    p = safe_snprintf(p, end, "{ \"now\" : %.1f,\n", now / 1000.0);

    //p = safe_snprintf(p, end, "  \"columns\" : [ \"receiverId\", \"\"],\n");
    p = safe_snprintf(p, end, "  \"receivers\" : [\n");

    struct receiver *r;

    for (int j = 0; j < RECEIVER_TABLE_SIZE; j++) {
        for (r = Modes.receiverTable[j]; r; r = r->next) {

            // check if we have enough space
            if ((p + 1000) >= end) {
                int used = p - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
            }

            char uuid[64];
            sprint_uuid1(r->id, uuid);

            double elapsed = (r->lastSeen - r->firstSeen) / 1000.0 + 1.0;
            p = safe_snprintf(p, end, "    [ \"%s\", %6.2f, %6.2f, %6.2f, %6.2f, %7.2f, %7.2f, %d, %0.2f,%0.2f ],\n",
                    uuid,
                    r->positionCounter / elapsed,
                    r->timedOutCounter * 3600.0 / elapsed,
                    r->latMin,
                    r->latMax,
                    r->lonMin,
                    r->lonMax,
                    r->badExtent ? 1 : 0,
                    r->latMin + (r->latMax - r->latMin) / 2.0,
                    r->lonMin + (r->lonMax - r->lonMin) / 2.0);

            if (p >= end)
                fprintf(stderr, "buffer overrun client json\n");
        }
    }

    if (*(p-2) == ',')
        *(p-2) = ' ';

    p = safe_snprintf(p, end, "  ]\n}\n");

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;
}
