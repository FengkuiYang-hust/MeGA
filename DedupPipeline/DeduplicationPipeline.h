/*
 * Author   : Xiangyu Zou
 * Date     : 04/23/2021
 * Time     : 15:39
 * Project  : MeGA
 This source code is licensed under the GPLv2
 */

#ifndef MEGA_DEDUPLICATIONPIPELINE_H
#define MEGA_DEDUPLICATIONPIPELINE_H


#include "jemalloc/jemalloc.h"
#include "../MetadataManager/MetadataManager.h"
#include "WriteFilePipeline.h"
#include <assert.h>
#include "../Utility/Likely.h"
#include "../xdelta/xdelta3.h"
//#include "../Utility/BaseCache.h"
#include "../Utility/BaseCache2.h"

struct BaseChunkPositions {
    uint64_t category: 22;
    uint64_t quantizedOffset: 42;
};

DEFINE_uint64(DeltaSelectorThreshold,
              10, "DeltaSelectorThreshold");

extern bool DeltaSwitch;
struct timeval initTime, endTime;

class DeduplicationPipeline {
public:
    DeduplicationPipeline()
            : taskAmount(0),
              runningFlag(true),
              mutexLock(),
              condition(mutexLock) {
        worker = new std::thread(std::bind(&DeduplicationPipeline::deduplicationWorkerCallback, this));

    }

    int addTask(const DedupTask &dedupTask) {
        MutexLockGuard mutexLockGuard(mutexLock);
        receiveList.push_back(dedupTask);
        taskAmount++;
        condition.notifyAll();
        return 0;
    }

    ~DeduplicationPipeline() {
        runningFlag = false;
        condition.notifyAll();
        worker->join();
        delete worker;
    }

    void getStatistics() {
        printf("[DedupDeduplicating] total : %lu, delta encoding : %lu\n", duration, deltaTime);
        printf("Unique:%lu, Internal:%lu, Adjacent:%lu, Delta:%lu, Reject:%lu\n", chunkCounter[0], chunkCounter[1],
               chunkCounter[2], chunkCounter[3], cappingReject);
        printf("xdeltaError:%lu\n", xdeltaError);
//        printf("Total Length : %lu, AfterDedup : %lu, AfterDelta: %lu, DedupRatio : %f, DeltaRatio : %f\n",
//               totalLength, afterDedup, afterDelta, (float) totalLength / afterDedup, (float) totalLength / afterDelta);
        GlobalMetadataManagerPtr->setTotalLength(totalLength);
        GlobalMetadataManagerPtr->setAfterDedup(afterDedup);
        GlobalMetadataManagerPtr->setAfterDelta(afterDelta);
    }


private:
    void deduplicationWorkerCallback() {
        pthread_setname_np(pthread_self(), "Dedup Thread");

        std::list<WriteTask> saveList;

        std::list<DedupTask> detectList;
        uint64_t segmentLength = 0;
        uint64_t SegmentThreshold = 20 * 1024 * 1024;

        while (likely(runningFlag)) {
            {
                MutexLockGuard mutexLockGuard(mutexLock);
                while (!taskAmount) {
                    condition.wait();
                    if (unlikely(!runningFlag)) return;
                }
                //printf("get task\n");
                taskAmount = 0;
                taskList.swap(receiveList);
            }

            if (newVersionFlag) {
                for (int i = 0; i < 4; i++) {
                    chunkCounter[i] = 0;
                }
                newVersionFlag = false;
                gettimeofday(&initTime, NULL);
                duration = 0;
                if (!taskList.empty()) {
                    baseCache.setCurrentVersion(taskList.begin()->fileID);
                }
            }

            for (const auto &dedupTask: taskList) {
                detectList.push_back(dedupTask);
                segmentLength += dedupTask.length;
                if (segmentLength > SegmentThreshold || dedupTask.countdownLatch) {

                    processingWaitingList(detectList);
                    deltaSelector(detectList);
                    doDedup(detectList);

                    baseCache.checkThreshold();

                    segmentLength = 0;
                    detectList.clear();
                }
            }
            taskList.clear();
        }

    }

    void processingWaitingList(std::list<DedupTask> &dl) {
        BasePos tempBasePos;
        BlockEntry2 tempBlockEntry;
        for (auto &entry: dl) {

            FPTableEntry fpTableEntry;
            LookupResult lookupResult = GlobalMetadataManagerPtr->dedupLookup(entry.fp, entry.length, &fpTableEntry);

            entry.lookupResult = lookupResult;

            if (lookupResult == LookupResult::Unique) {
                LookupResult similarLookupResult = LookupResult::Dissimilar;
                odessCalculation(entry.buffer + entry.pos, entry.length, &entry.similarityFeatures);
                if (DeltaSwitch) {
                    SHA1FP targetChunk;
                    int r = baseCache.findRecord(entry.similarityFeatures, &targetChunk);
                    if(r) {
                        entry.lookupResult = similarLookupResult;
                        entry.basePos.sha1Fp = targetChunk;
                        entry.inCache = r;
                    }else{
                        similarLookupResult = GlobalMetadataManagerPtr->similarityLookupSimple(entry.similarityFeatures,
                                                                                               &tempBasePos);
                        if (similarLookupResult == LookupResult::Similar) {
                            entry.lookupResult = similarLookupResult;
                            entry.basePos = tempBasePos;
                            entry.inCache = false;
                        }
                    }
                }
            } else if (lookupResult == LookupResult::InternalDedup) {
                // do nothing
            } else if (lookupResult == LookupResult::InternalDeltaDedup) {
                // do nothing
            } else if (lookupResult == LookupResult::AdjacentDedup) {
                // do nothing
            }
        }
    }

    void deltaSelector(std::list<DedupTask> &dl) {
        std::unordered_map<uint64_t, uint64_t> baseChunkPositions;
        for (auto &entry: dl) {
            if (entry.lookupResult == LookupResult::Similar && entry.inCache == 0) {
                uint64_t key;
                BaseChunkPositions *bcp = (BaseChunkPositions *) &key;
                bcp->category = entry.basePos.CategoryOrder;
                bcp->quantizedOffset = entry.basePos.cid;
                auto iter = baseChunkPositions.find(key);
                if (iter == baseChunkPositions.end()) {
                    baseChunkPositions.insert({key, 1});
                } else {
                    baseChunkPositions[key]++;
                }
            }
        }
        for (auto &entry: baseChunkPositions) {
            if (entry.second < FLAGS_DeltaSelectorThreshold) {
                entry.second = 0;
            }
        }
        for (auto &entry: dl) {
            if (entry.lookupResult == LookupResult::Similar && entry.inCache == 0) {
                uint64_t key;
                BaseChunkPositions *bcp = (BaseChunkPositions *) &key;
                bcp->category = entry.basePos.CategoryOrder;
                bcp->quantizedOffset = entry.basePos.cid;
                if (baseChunkPositions[key] == 0) {
                    entry.deltaReject = true;
                }
            }
        }

    }

    void doDedup(std::list<DedupTask> &dl) {
        WriteTask writeTask;
        BlockEntry2 tempBlockEntry;
        struct timeval t0, t1, dt1, dt2;

        for (auto &entry: dl) {
            gettimeofday(&t0, NULL);
            memset(&writeTask, 0, sizeof(WriteTask));

            FPTableEntry fpTableEntry;
            LookupResult lookupResult = GlobalMetadataManagerPtr->dedupLookup(entry.fp, entry.length, &fpTableEntry);

            writeTask.fileID = entry.fileID;
            writeTask.index = entry.index;
            writeTask.buffer = entry.buffer;
            writeTask.pos = entry.pos;
            writeTask.length = entry.length;
            writeTask.sha1Fp = entry.fp;
            writeTask.deltaTag = 0;

            totalLength += entry.length;

            writeTask.type = (int) lookupResult;

            if (lookupResult == LookupResult::Unique) {
                afterDedup += entry.length;
                LookupResult similarLookupResult = LookupResult::Dissimilar;
                int cacheSearch;
                if (DeltaSwitch) {
                    cacheSearch = baseCache.findRecord(entry.similarityFeatures, &(entry.basePos.sha1Fp));
                    if (cacheSearch) {
                        similarLookupResult = LookupResult::Similar;
                    } else {
                        similarLookupResult = GlobalMetadataManagerPtr->similarityLookupSimple(entry.similarityFeatures,
                                                                                               &entry.basePos);
                    }
                }
                if (similarLookupResult == LookupResult::Similar && !entry.deltaReject) {
                    int r;
                    if (r == 0) {
                        r = baseCache.tryGetRecord(&entry.basePos, &tempBlockEntry);
                        if (!r) {
                            baseCache.loadBaseChunks(entry.basePos);
                            r = baseCache.getRecord(&entry.basePos, &tempBlockEntry);
                            assert(r);
                        }
                    }


                    // calculate delta
                    uint8_t *tempBuffer = (uint8_t *) malloc(65536);
                    usize_t deltaSize;
                    gettimeofday(&dt1, NULL);

                    r = xd3_encode_memory(entry.buffer + entry.pos, entry.length,
                                          tempBlockEntry.block, tempBlockEntry.length, tempBuffer, &deltaSize,
                                          entry.length, XD3_COMPLEVEL_1 | XD3_NOCOMPRESS);
                    gettimeofday(&dt2, NULL);
                    deltaTime += (dt2.tv_sec - dt1.tv_sec) * 1000000 + dt2.tv_usec - dt1.tv_usec;

                    if (r != 0 || deltaSize >= entry.length) {
                        // no delta
                        free(tempBuffer);
                        xdeltaError++;
                        goto unique;
                    } else {
                        // add metadata
                        GlobalMetadataManagerPtr->deltaAddRecord(writeTask.sha1Fp, entry.fileID,
                                                                 entry.basePos.sha1Fp,
                                                                 entry.length - deltaSize,
                                                                 entry.length);
                        // extend base lifecycle
                        FPTableEntry tFTE = {
                                0,
                                entry.basePos.CategoryOrder,
                                entry.basePos.length,
                                entry.basePos.length
                        };
                        GlobalMetadataManagerPtr->extendBase(entry.basePos.sha1Fp, tFTE);
                        // update task
                        writeTask.type = (int) similarLookupResult;
                        writeTask.buffer = tempBuffer;
                        writeTask.pos = 0;
                        writeTask.length = deltaSize;
                        writeTask.oriLength = entry.length;
                        writeTask.deltaTag = 1;
                        writeTask.baseFP = entry.basePos.sha1Fp;
                        lastCategoryLength += deltaSize + sizeof(BlockHeader);
                        if (lastCategoryLength >= ContainerSize) {
                            lastCategoryLength = 0;
                            currentCID++;
                            baseCache.endCurrentContainer();
                        }
                        chunkCounter[3]++;
                        afterDelta += deltaSize;
                    }
                } else {
                    unique:
                    chunkCounter[(int) LookupResult::Unique]++;
                    if (entry.deltaReject) cappingReject++;
                    writeTask.type = (int) LookupResult::Unique;
                    GlobalMetadataManagerPtr->uniqueAddRecord(entry.fp, entry.fileID, entry.length);
                    GlobalMetadataManagerPtr->addSimilarFeature(entry.similarityFeatures,
                                                                {entry.fp, (uint32_t) entry.fileID,
                                                                 currentCID, entry.length});
                    writeTask.similarityFeatures = entry.similarityFeatures;
                    baseCache.addRecentRecord(writeTask.sha1Fp, writeTask.buffer + writeTask.pos, writeTask.length,
                                              entry.similarityFeatures);
                    lastCategoryLength += entry.length + sizeof(BlockHeader);
                    if (lastCategoryLength >= ContainerSize) {
                        currentCID++;
                        baseCache.endCurrentContainer();
                        lastCategoryLength = 0;
                    }
                    afterDelta += entry.length;
                }
            } else if (lookupResult == LookupResult::InternalDedup) {
                chunkCounter[(int) lookupResult]++;
                // nothing to do
            } else if (lookupResult == LookupResult::InternalDeltaDedup) {
                chunkCounter[(int) LookupResult::InternalDedup]++;
                writeTask.type = (int) LookupResult::InternalDedup;
                writeTask.oriLength = fpTableEntry.oriLength; //updated
                writeTask.baseFP = fpTableEntry.baseFP;
                writeTask.deltaTag = 1;
                writeTask.length = fpTableEntry.length;
            } else if (lookupResult == LookupResult::AdjacentDedup) {
                chunkCounter[(int) lookupResult]++;
                GlobalMetadataManagerPtr->neighborAddRecord(writeTask.sha1Fp, fpTableEntry);
                if (fpTableEntry.deltaTag) {
                    writeTask.length = fpTableEntry.length;
                    writeTask.deltaTag = 1;
                    writeTask.baseFP = fpTableEntry.baseFP;
                    writeTask.oriLength = fpTableEntry.oriLength; //updated
                    FPTableEntry tFTE = {
                            0,
                            fpTableEntry.categoryOrder
                    };
                    GlobalMetadataManagerPtr->neighborAddRecord(fpTableEntry.baseFP, tFTE);
                }
            }

            gettimeofday(&t1, NULL);
            duration += (t1.tv_sec - t0.tv_sec) * 1000000 + t1.tv_usec - t0.tv_usec;

            if (unlikely(entry.countdownLatch)) {
                printf("DedupPipeline finish\n");
                writeTask.countdownLatch = entry.countdownLatch;
                entry.countdownLatch->countDown();
                //GlobalMetadataManagerPtr->tableRolling();
                newVersionFlag = true;
                gettimeofday(&endTime, NULL);
                printf("[CheckPoint:dedup] InitTime:%lu, EndTime:%lu\n", initTime.tv_sec * 1000000 + initTime.tv_usec,
                       endTime.tv_sec * 1000000 + endTime.tv_usec);
                baseCache.endCurrentContainer();

                GlobalWriteFilePipelinePtr->addTask(writeTask);
            } else {
                GlobalWriteFilePipelinePtr->addTask(writeTask);
            }

            writeTask.countdownLatch = nullptr;

        }

    }

    std::thread *worker;
    std::list<DedupTask> taskList;
    std::list<DedupTask> receiveList;
    int taskAmount;
    bool runningFlag;
    MutexLock mutexLock;
    Condition condition;

    BaseCache2 baseCache;

    uint64_t totalLength = 0;
    uint64_t afterDedup = 0;
    uint64_t afterDelta = 0;
    uint64_t lastCategoryLength = 0;
    uint64_t currentCID = 0;

    uint64_t chunkCounter[4] = {0, 0, 0, 0};
    uint64_t xdeltaError = 0;

    uint64_t duration = 0;
    uint64_t deltaTime = 0;

    uint64_t cappingReject = 0;

    bool newVersionFlag = true;
};

static DeduplicationPipeline *GlobalDeduplicationPipelinePtr;

#endif //MEGA_DEDUPLICATIONPIPELINE_H
