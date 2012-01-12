#include "dht/Address.h"
#include "dht/dhtcore/DistanceNodeCollector.h"
#include "dht/dhtcore/Node.h"
#include "dht/dhtcore/NodeHeader.h"
#include "dht/dhtcore/NodeStore.h"
#include "dht/dhtcore/NodeStore_struct.h"
#include "dht/dhtcore/NodeCollector.h"
#include "dht/dhtcore/NodeList.h"
#include "log/Log.h"
#include "switch/NumberCompress.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/** See: NodeStore.h */
struct NodeStore* NodeStore_new(struct Address* myAddress,
                                const uint32_t capacity,
                                const struct MemAllocator* allocator,
                                struct Log* logger)
{
    struct NodeStore* out = allocator->malloc(sizeof(struct NodeStore), allocator);
    out->thisNodeAddress = myAddress;
    out->headers = allocator->malloc(sizeof(struct NodeHeader) * capacity, allocator);
    out->nodes = allocator->malloc(sizeof(struct Node) * capacity, allocator);
    out->capacity = capacity;
    out->logger = logger;
    out->size = 0;
    return out;
}

/** See: NodeStore.h */
struct Node* NodeStore_getNode(const struct NodeStore* store, struct Address* addr)
{
    uint32_t pfx = Address_getPrefix(addr);

    // If multiple nodes with the same address, get the one with the best reach.
    int32_t bestIndex = -1;
    uint32_t bestReach = 0;
    for (int32_t i = 0; i < (int32_t) store->size; i++) {
        if (pfx == store->headers[i].addressPrefix
            && memcmp(addr->key, store->nodes[i].address.key, Address_KEY_SIZE) == 0
            && store->headers[i].reach >= bestReach)
        {
            bestIndex = i;
            bestReach = store->headers[i].reach;
        }
    }

    if (bestIndex == -1) {
        return NULL;
    }

    // Synchronize the reach values.
    store->nodes[bestIndex].reach = store->headers[bestIndex].reach;
    return &store->nodes[bestIndex];
}

static inline uint32_t getSwitchIndex(struct Address* addr)
{
    uint64_t label = Endian_bigEndianToHost64(addr->networkAddress_be);
    uint32_t bits = NumberCompress_bitsUsedForLabel(label);
    return NumberCompress_getDecompressed(label, bits);
}

static inline void replaceNode(struct Node* const nodeToReplace,
                               struct NodeHeader* const headerToReplace,
                               struct Address* addr)
{
    headerToReplace->addressPrefix = Address_getPrefix(addr);
    headerToReplace->reach = 0;
    headerToReplace->switchIndex = getSwitchIndex(addr);
    memcpy(&nodeToReplace->address, addr, sizeof(struct Address));
}

static inline void adjustReach(struct NodeHeader* header,
                               const int64_t reachDiff)
{
    if (reachDiff == 0) {
        return;
    }
    int64_t newReach = reachDiff + header->reach;
    if (newReach < 0) {
        header->reach = 0;
    } else if (newReach > INT32_MAX) {
        header->reach = INT32_MAX;
    } else {
        header->reach = (uint32_t) newReach;
    }
}

void NodeStore_addNode(struct NodeStore* store,
                       struct Address* addr,
                       const int64_t reachDifference)
{
    Address_getPrefix(addr);
    if (memcmp(addr->ip6.bytes, store->thisNodeAddress, 16) == 0) {
        printf("got introduced to ourselves\n");
        return;
    }
    if (addr->ip6.bytes[0] != 0xfc) {
        uint8_t address[60];
        Address_print(address, addr);
        Log_critical1(store->logger,
                      "tried to insert address %s which does not begin with 0xFC.\n",
                      address);
        assert(false);
    }

    // TODO: maintain a sorted list.

    uint32_t pfx = Address_getPrefix(addr);
    if (store->size < store->capacity) {
        for (uint32_t i = 0; i < store->size; i++) {
            if (store->headers[i].addressPrefix == pfx
                && Address_isSameIp(&store->nodes[i].address, addr))
            {
                int red = Address_checkRedundantRoute(&store->nodes[i].address, addr);
                if (red == 1) {
                    #ifdef Log_DEBUG
                        uint8_t nodeAddr[60];
                        Address_print(nodeAddr, &store->nodes[i].address);
                        uint8_t newAddr[20];
                        Address_printNetworkAddress(newAddr, addr);
                        Log_debug2(store->logger,
                                   "Found a better route to %s via %s\n",
                                   nodeAddr,
                                   newAddr);

                        struct Node* n =
                            NodeStore_getNodeByNetworkAddr(addr->networkAddress_be, store);
                        if (n) {
                            Log_warn(store->logger, "This route is probably invalid, giving up.\n");
                            continue;
                        }
                    #endif
                    store->nodes[i].address.networkAddress_be = addr->networkAddress_be;
                } else if (red == 0
                    && store->nodes[i].address.networkAddress_be != addr->networkAddress_be)
                {
                    // Completely different routes, store seperately.
                    continue;
                }

                /*#ifdef Log_DEBUG
                    uint32_t oldReach = store->headers[i].reach;
                #endif*/

                adjustReach(&store->headers[i], reachDifference);

                /*#ifdef Log_DEBUG
                    if (oldReach != store->headers[i].reach) {
                        uint8_t nodeAddr[60];
                        Address_print(nodeAddr, addr);
                        Log_debug3(store->logger,
                                   "Altering reach for node %s, old reach %u, new reach %u.\n",
                                   nodeAddr,
                                   oldReach,
                                   store->headers[i].reach);
                        if (oldReach > store->headers[i].reach) {
                            Log_debug(store->logger, "Reach was decreased!\n");
                        }
                    }
                #endif*/

                return;
            }
            #ifdef Log_DEBUG
                else if (store->headers[i].addressPrefix == pfx) {
                    uint8_t realAddr[16];
                    AddressCalc_addressForPublicKey(realAddr, addr->key);
                    assert(!memcmp(realAddr, addr->ip6.bytes, 16));
                }
            #endif
        }

        #ifdef Log_DEBUG
            uint8_t nodeAddr[60];
            Address_print(nodeAddr, addr);
            Log_debug2(store->logger,
                       "Discovered node: %s reach %u\n",
                       nodeAddr,
                       reachDifference);
        #endif

        // Free space, regular insert.
        replaceNode(&store->nodes[store->size], &store->headers[store->size], addr);
        adjustReach(&store->headers[store->size], reachDifference);
        store->size++;
        return;
    }

    // The node whose reach OR distance is the least.
    // This means nodes who are close and have short reach will be removed
    uint32_t indexOfNodeToReplace = 0;
    uint32_t leastReachOrDistance = UINT32_MAX;
    for (uint32_t i = 0; i < store->size; i++) {

        uint32_t distance = store->headers[i].addressPrefix ^ pfx;

        if (distance == 0 && Address_isSame(&store->nodes[i].address, addr)) {
            // Node already exists
            adjustReach(&store->headers[store->size], reachDifference);
            return;
        }

        uint32_t reachOrDistance = store->headers[i].reach | distance;

        if (reachOrDistance < leastReachOrDistance) {
            leastReachOrDistance = reachOrDistance;
            indexOfNodeToReplace = i;
        }
    }

    replaceNode(&store->nodes[indexOfNodeToReplace],
                &store->headers[indexOfNodeToReplace],
                addr);

    adjustReach(&store->headers[indexOfNodeToReplace], reachDifference);
}

static struct Node* nodeForHeader(struct NodeHeader* header, struct NodeStore* store)
{
    struct Node* n = &store->nodes[header - store->headers];
    n->reach = header->reach;
    return n;
}

struct Node* NodeStore_getBest(struct Address* targetAddress, struct NodeStore* store)
{
    struct NodeCollector_Element element = {
        .value = 0,
        .distance = UINT32_MAX,
        .node = NULL
    };

    struct NodeCollector collector = {
        .capacity = 1,
        .targetPrefix = Address_getPrefix(targetAddress),
        .targetAddress = targetAddress,
        .nodes = &element,
        .logger = store->logger
    };

    collector.thisNodeDistance =
        Address_getPrefix(store->thisNodeAddress) ^ collector.targetPrefix;

    for (uint32_t i = 0; i < store->size; i++) {
        NodeCollector_addNode(store->headers + i, store->nodes + i, &collector);
    }

    return element.node ? nodeForHeader(element.node, store) : NULL;
}

struct NodeList* NodeStore_getNodesByAddr(struct Address* address,
                                          const uint32_t max,
                                          const struct MemAllocator* allocator,
                                          struct NodeStore* store)
{
    struct NodeCollector* collector = NodeCollector_new(address,
                                                        max,
                                                        store->thisNodeAddress,
                                                        true,
                                                        store->logger,
                                                        allocator);

    for (uint32_t i = 0; i < store->size; i++) {
        DistanceNodeCollector_addNode(store->headers + i, store->nodes + i, collector);
    }

    struct NodeList* out = allocator->malloc(sizeof(struct NodeList), allocator);
    out->nodes = allocator->malloc(max * sizeof(char*), allocator);

    uint32_t outIndex = 0;
    for (uint32_t i = 0; i < max; i++) {
        if (collector->nodes[i].node != NULL
            && !memcmp(collector->nodes[i].body->address.ip6.bytes, address->ip6.bytes, 16))
        {
            out->nodes[outIndex] = collector->nodes[i].body;
            outIndex++;
        }
    }
    out->size = outIndex;

    return out;
}

/** See: NodeStore.h */
struct NodeList* NodeStore_getClosestNodes(struct NodeStore* store,
                                           struct Address* targetAddress,
                                           struct Address* requestorsAddress,
                                           const uint32_t count,
                                           const bool allowNodesFartherThanUs,
                                           const struct MemAllocator* allocator)
{
    struct NodeCollector* collector = NodeCollector_new(targetAddress,
                                                        count,
                                                        store->thisNodeAddress,
                                                        allowNodesFartherThanUs,
                                                        store->logger,
                                                        allocator);

    // Don't send nodes which route back to the node which asked us.
    uint32_t index = (requestorsAddress) ? getSwitchIndex(requestorsAddress) : 0;

    // naive implementation, todo make this faster
    for (uint32_t i = 0; i < store->size; i++) {
        if (requestorsAddress && store->headers[i].switchIndex == index) {
            continue;
        }
        NodeCollector_addNode(store->headers + i, store->nodes + i, collector);
    }

    struct NodeList* out = allocator->malloc(sizeof(struct NodeList), allocator);
    out->nodes = allocator->malloc(count * sizeof(char*), allocator);

    uint32_t outIndex = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (collector->nodes[i].node != NULL) {
            out->nodes[outIndex] = nodeForHeader(collector->nodes[i].node, store);
            outIndex++;
        }
    }
    out->size = outIndex;

    return out;
}

/** See: NodeStore.h */
void NodeStore_updateReach(const struct Node* const node,
                           const struct NodeStore* const store)
{
    store->headers[node - store->nodes].reach = node->reach;
}

uint32_t NodeStore_size(const struct NodeStore* const store)
{
    return store->size;
}

struct Node* NodeStore_getNodeByNetworkAddr(uint64_t networkAddress_be, struct NodeStore* store)
{
    for (uint32_t i = 0; i < store->size; i++) {
        if (networkAddress_be == store->nodes[i].address.networkAddress_be) {
            return &store->nodes[i];
        }
    }
    return NULL;
}

void NodeStore_dumpTables(struct Writer* writer, struct NodeStore* store)
{
    for (uint32_t i = 0; i < store->size; i++) {
        uint8_t out[60];
        Address_print(out, &store->nodes[i].address);
        writer->write(out, 60, writer);
        char reachDec[48];
        snprintf(reachDec, 48, " reach = %u\n", store->headers[i].reach);
        writer->write(reachDec, strlen(reachDec), writer);
    }
}

void NodeStore_remove(struct Node* node, struct NodeStore* store)
{
    assert(node >= store->nodes && node < store->nodes + store->size);
    store->size--;
    memcpy(node, &store->nodes[store->size], sizeof(struct Node));
    struct NodeHeader* header = &store->headers[node - store->nodes];
    memcpy(header, &store->headers[store->size], sizeof(struct NodeHeader));
}
