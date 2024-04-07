//
// Created by Borelset on 2023/7/1.
//

#ifndef MEGA_BASECACHE2_H
#define MEGA_BASECACHE2_H

#include <unordered_map>
#include <map>

uint64_t PreloadSize = ContainerSize * 1.2;

extern std::string ClassFileAppendPath;

DEFINE_uint64(CacheSize,
              128, "CappingThreshold");

uint64_t TotalSizeThreshold = FLAGS_CacheSize;

int UpdateScore = 2;

struct BlockEntry2 {
    uint8_t *block = nullptr;
    uint64_t length = 0;
};

class ContainerCacheEntry {
public:
    ContainerCacheEntry() {

    }

    ~ContainerCacheEntry() {
        for (auto &block: chunkTable) {
            free(block.second.block);
        }
    }

    void addRecord(const SHA1FP &sha1Fp, uint8_t *buffer, uint64_t length, SimilarityFeatures features) {
        //MutexLockGuard cacheLockGuard(cacheLock);
        auto iter = chunkTable.find(sha1Fp);
        if (iter == chunkTable.end()) {
            uint8_t *cacheBuffer = (uint8_t *) malloc(length);
            memcpy(cacheBuffer, buffer, length);
            chunkTable[sha1Fp] = {
                    cacheBuffer, length
            };
            featureTable[0][features.feature1].insert(sha1Fp);
            featureTable[1][features.feature2].insert(sha1Fp);
            featureTable[2][features.feature3].insert(sha1Fp);
            totalSize += length;
        }
    }

    int findRecord(const SimilarityFeatures &features, SHA1FP *sha1Fp) const {
        int r = 0;

        auto iter1 = featureTable[0].find(features.feature1);
        if (iter1 != featureTable[0].end()) {
            *sha1Fp = *(iter1->second.begin());
            return 1;
        }

        auto iter2 = featureTable[1].find(features.feature2);
        if (iter2 != featureTable[1].end()) {
            *sha1Fp = *(iter2->second.begin());
            return 1;
        }

        auto iter3 = featureTable[2].find(features.feature3);
        if (iter3 != featureTable[2].end()) {
            *sha1Fp = *(iter3->second.begin());
            return 1;
        }

        return 0;
    }

    int getRecord(const SHA1FP &sha1Fp, BlockEntry2 *block) {
        block->block = chunkTable[sha1Fp].block;
        block->length = chunkTable[sha1Fp].length;
        return 1;
    }

    int tryGetRecord(const SHA1FP &sha1Fp, BlockEntry2 *block) const {
        auto iter = chunkTable.find(sha1Fp);
        if (iter != chunkTable.end()) {
            block->block = iter->second.block;
            block->length = iter->second.length;
            return 1;
        }
        return 0;
    }

    uint64_t lastVisit = 0;
    uint64_t score = 0;
    uint64_t totalSize = 0;

    void move(ContainerCacheEntry &old) {
        chunkTable.swap(old.chunkTable);
        for (int i = 0; i < 3; i++) {
            featureTable[i].swap(old.featureTable[i]);
        }
        lastVisit = old.lastVisit;
        score = old.score;
        totalSize = old.totalSize;
    }

    void clear() {
        chunkTable.clear();
        for (int i = 0; i < 3; i++) {
            featureTable[i].clear();
        }
        lastVisit = 0;
        score = 0;
        totalSize = 0;
    }

private:
    std::unordered_map<SHA1FP, BlockEntry2, TupleHasher, TupleEqualer> chunkTable;
    std::unordered_map<uint64_t, std::unordered_set<SHA1FP, TupleHasher, TupleEqualer>> featureTable[3];
};

class BaseCache2 {
public:
    BaseCache2() : totalSize(0), index(0), cacheMap(65536), write(0), read(0) {
        preloadBuffer = (uint8_t *) malloc(PreloadSize);
        decompressBuffer = (uint8_t *) malloc(PreloadSize);
        TotalSizeThreshold = FLAGS_CacheSize;
    }

    void setCurrentVersion(uint64_t verison) {
        currentVersion = verison;
    }

    ~BaseCache2() {
        statistics();
        free(preloadBuffer);
    }

    void statistics() {
        printf("block cache:\n");
        printf("total size:%lu, items:%lu\n", totalSize, items);
        printf("cache write:%lu, cache read:%lu\n", write, read);
        printf("hit rate: %f(%lu/%lu)\n", float(success) / access, success, access);
        printf("cache miss %lu times, total loading time %lu us, average %f us\n", access - success, loadingTime,
               (float) loadingTime / (access - success));
        printf("self hit:%lu\n", selfHit);
    }

    void loadBaseChunks(const BasePos &basePos) {
        gettimeofday(&t0, NULL);
        char pathBuffer[256];
        uint64_t targetCategory;
        uint64_t readSize = 0;
        int r = 0;

        if (basePos.CategoryOrder == currentVersion) {
            sprintf(pathBuffer, ClassFilePath.data(), basePos.CategoryOrder, currentVersion, basePos.cid);
            selfHit++;
        } else if (basePos.CategoryOrder) {
            sprintf(pathBuffer, ClassFilePath.data(), basePos.CategoryOrder, currentVersion - 1, basePos.cid);
        } else {
            sprintf(pathBuffer, ClassFileAppendPath.data(), 1, currentVersion - 1, basePos.cid);
        }


        {
            FileOperator basefile(pathBuffer, FileOpenType::Read);
            if(basefile.getStatus() == -1){
                r = GlobalWriteFilePipelinePtr->getContainer(basePos.CategoryOrder, basePos.CategoryOrder, basePos.cid, preloadBuffer, &readSize);
                assert(r);
                printf("[cid:%lu] Try get from writer success\n", basePos.cid);
            }else{
                readSize = basefile.read(decompressBuffer, PreloadSize);
                //printf("[cid:%lu] readsize:%lu\n", basePos.cid, readSize);
                basefile.releaseBufferedData();

                readSize = ZSTD_decompress(preloadBuffer, PreloadSize, decompressBuffer, readSize);
                assert(!ZSTD_isError(readSize));
                assert(basePos.length <= readSize);
            }
        }

        BlockHeader *headPtr;

        uint64_t preLoadPos = 0;
        uint64_t leftLength = readSize;
        uint64_t containerSize = 0;

        uint64_t cacheID = basePos.CategoryOrder;
        cacheID = (cacheID << 32) + basePos.cid;
        assert(cacheMap.find(cacheID) == cacheMap.end());
        ContainerCacheEntry &newContainerCache = cacheMap[cacheID];
        cacheMap[cacheID].lastVisit = index;
        lruList[index] = cacheID;
      //printf("[Reload] cid:%lu, lastVisit: %lu\n", cacheID, index);
        index++;

        while (leftLength > sizeof(BlockHeader) &&
               leftLength >= (2048 + sizeof(BlockHeader))) {// todo: min chunksize configured to 2048
            headPtr = (BlockHeader *) (preloadBuffer + preLoadPos);
            if (headPtr->length + sizeof(BlockHeader) > leftLength) {
                break;
            } else if (!headPtr->type) {
//          addRecord(headPtr->fp, preloadBuffer + preLoadPos + sizeof(BlockHeader),
//                    headPtr->length);
                newContainerCache.addRecord(headPtr->fp, preloadBuffer + preLoadPos + sizeof(BlockHeader),
                                            headPtr->length, headPtr->sFeatures);
                containerSize += headPtr->length;
            }

            preLoadPos += headPtr->length + sizeof(BlockHeader);
            if (preLoadPos >= readSize) break;
            leftLength = readSize - preLoadPos;
        }
        newContainerCache.totalSize = containerSize;
        gettimeofday(&t1, NULL);
        loadingTime += (t1.tv_sec - t0.tv_sec) * 1000000 + t1.tv_usec - t0.tv_usec;
        write += newContainerCache.totalSize;

        totalSize += newContainerCache.totalSize;
        items++;

        checkThreshold();
    }

    int addRecentRecord(const SHA1FP &sha1Fp, uint8_t *buffer, uint64_t length, const SimilarityFeatures &features) {
        currentContainer.addRecord(sha1Fp, buffer, length, features);
        return 0;
    }

    int findRecord(const SimilarityFeatures &features, SHA1FP *targetChunk) {

        int r = currentContainer.findRecord(features, targetChunk);
        if (r == 1) {
            return r;
        }

        for (const auto &table: cacheMap) {
            r = table.second.findRecord(features, targetChunk);
            if (r == 1) {
                return r;
            }
        }
        return 0;
    }

    int tryGetRecord(const BasePos *basePos, BlockEntry2 *block) {
        access++;
        int r = currentContainer.tryGetRecord(basePos->sha1Fp, block);
        if (r == 1) {
            read += block->length;
            success++;
            return r;
        }

        for (const auto &table: cacheMap) {
            r = table.second.tryGetRecord(basePos->sha1Fp, block);
            if (r == 1) {
                success++;
                freshLastVisit(table.first);
                read += block->length;
                return r;
            }
        }

        return 0;
    }

    int getRecord(const BasePos *basePos, BlockEntry2 *block) {
        int r = currentContainer.tryGetRecord(basePos->sha1Fp, block);
        if (r == 1) {
            read += block->length;
            return r;
        }

        for (const auto &table: cacheMap) {
            r = table.second.tryGetRecord(basePos->sha1Fp, block);
            if (r == 1) {
                freshLastVisit(table.first);
                read += block->length;
                return r;
            }
        }

        return 0;
    }

    int endCurrentContainer() {
        uint64_t cacheID = (currentVersion << 32) + cid;
        cid++;
        write += currentContainer.totalSize;
        items++;
        totalSize += currentContainer.totalSize;

        cacheMap[cacheID].move(currentContainer);
        currentContainer.clear();
        cacheMap[cacheID].lastVisit = index;
        //printf("[New] cid:%lu, lastVisit: %lu\n", cacheID, index);

        lruList[index] = cacheID;
        index++;

        checkThreshold();
        return 0;
    }

    void checkThreshold() {
        while (items > TotalSizeThreshold) {
            auto iterLru = lruList.begin();
            assert(iterLru != lruList.end());
            //printf("[Eliminate] cid:%lu, lastvisit: %lu\n", iterLru->second, iterLru->first);
            auto iterCache = cacheMap.find(iterLru->second);
            assert(iterCache != cacheMap.end());
            totalSize -= iterCache->second.totalSize;
            cacheMap.erase(iterCache);
            lruList.erase(iterLru);
            items--;
        }
    }

private:


    void freshLastVisit(uint64_t cid) {
        cacheMap[cid].score++;
        if (cacheMap[cid].score > UpdateScore) {
            cacheMap[cid].score = 0;
        //printf("[Fresh old] cid:%lu, lastVisit: %lu\n", cid, cacheMap[cid].lastVisit);
            auto iter_l = lruList.find(cacheMap[cid].lastVisit);
            lruList[index] = iter_l->second;
        //printf("[Fresh new] cid:%lu, old: %lu, new: %lu\n", iter_l->second, cacheMap[cid].lastVisit, index);
            lruList.erase(iter_l);
            cacheMap[cid].lastVisit = index;
            index++;
        }
    }

    struct timeval t0, t1;
    uint64_t index;
    uint64_t totalSize;
    std::unordered_map<uint64_t, ContainerCacheEntry> cacheMap;
    std::map<uint64_t, uint64_t> lruList;
    ContainerCacheEntry currentContainer;
    uint64_t write, read;
    uint64_t access = 0, success = 0;
    uint64_t loadingTime = 0;
    uint64_t items = 0;
    uint64_t currentVersion = 0;
    uint64_t selfHit = 0;

    uint32_t cid = 0;

    uint8_t *preloadBuffer;
    uint8_t *decompressBuffer;
};

#endif //MEGA_BASECACHE2_H
