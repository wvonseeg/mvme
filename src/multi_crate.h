#ifndef __MVME_MULTI_CRATE_H__
#define __MVME_MULTI_CRATE_H__

#include <memory>
#include <set>

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cassert>
#include <stdexcept>
#include <fmt/format.h>
#include <mesytec-mvlc/mesytec-mvlc.h>

#include "vme_config.h"
#include "mvlc/mvlc_vme_controller.h"

namespace multi_crate
{

// XXX: leftoff here!
class MulticrateVMEConfig: public ConfigObject
{
    Q_OBJECT
    signals:
        void vmeConfigAdded(VMEConfig *cfg);
        void vmeConfigAboutToBeRemoved(VMEConfig *cfg);

    public:
        Q_INVOKABLE explicit MulticrateVMEConfig(QObject *parent = nullptr);

        void addVMEConfig(VMEConfig *cfg);
        bool removeVMEConfig(const VMEConfig *cfg);
        bool contains(const VMEConfig *cfg) const;

    private:
        std::vector<VMEConfig *> m_crateConfigs;
        std::set<int> m_crossCrateEventIndexes;
        std::vector<QUuid> m_eventMainModuleIds;
        VMEConfig *m_mergedConfig;
};

//
// VME config merging
//

struct MultiCrateModuleMappings
{
    QMap<QUuid, QUuid> cratesToMerged;
    QMap<QUuid, QUuid> mergedToCrates;

    void insertMapping(const ModuleConfig *crateModule, const ModuleConfig *mergedModule)
    {
        insertMapping(crateModule->getId(), mergedModule->getId());
    }

    void insertMapping(const QUuid &crateModuleId, const QUuid &mergedModuleId)
    {
        cratesToMerged.insert(crateModuleId, mergedModuleId);
        mergedToCrates.insert(mergedModuleId, crateModuleId);
    }
};

// inputs:
// * list of crate vme configs with the first being the main crate
// * list of event indexes which are part of a cross-crate event
//
// outputs:
// * a new merged vme config containing both merged cross-crate events and
//   non-merged single-crate events. The latter events are in linear (crate,
//   event) order.
// * bi-directional module id mappings
std::pair<std::unique_ptr<VMEConfig>, MultiCrateModuleMappings> make_merged_vme_config(
    const std::vector<VMEConfig *> &crateConfigs,
    const std::set<int> &crossCrateEvents
    );

inline std::pair<std::unique_ptr<VMEConfig>, MultiCrateModuleMappings> make_merged_vme_config(
    const std::vector<std::unique_ptr<VMEConfig>> &crateConfigs,
    const std::set<int> &crossCrateEvents
    )
{
    std::vector<VMEConfig *> rawConfigs;
    for (auto &ptr: crateConfigs)
        rawConfigs.emplace_back(ptr.get());

    return make_merged_vme_config(rawConfigs, crossCrateEvents);
}

//
// MultiCrateConfig
//

struct MultiCrateConfig
{
    // Filename of the VMEConfig for the main crate.
    QString mainConfig;

    // Filenames of VMEconfigs of the secondary crates.
    QStringList secondaryConfigs;

    // Event ids from mainConfig which form cross crate events.
    std::set<QUuid> crossCrateEventIds;

    // Ids of the "main" module for each cross crate event. The moduleId may
    // come from any of the individual VMEConfigs. The representation of this
    // module in the merged VMEConfig will be used as the EventBuilders main
    // module.
    std::set<QUuid> mainModuleIds;
};

inline bool operator==(const MultiCrateConfig &a, const MultiCrateConfig &b)
{
    return (a.mainConfig == b.mainConfig
            && a.secondaryConfigs == b.secondaryConfigs
            && a.crossCrateEventIds == b.crossCrateEventIds
           );
}

inline bool operator!=(const MultiCrateConfig &a, const MultiCrateConfig &b)
{
    return !(a == b);
}

MultiCrateConfig load_multi_crate_config(const QJsonDocument &doc);
MultiCrateConfig load_multi_crate_config(const QJsonObject &json);
MultiCrateConfig load_multi_crate_config(const QString &filename);

QJsonObject to_json_object(const MultiCrateConfig &mcfg);
QJsonDocument to_json_document(const MultiCrateConfig &mcfg);

//
// Playground (XXX: moved from the implementation file)
//

using namespace mesytec;

// listfile::WriteHandle implementation which enqueues the data onto the given
// ReadoutBufferQueues. The queues are used in a blocking fashion which means
// write() will block if the consuming side is too slow.
//
// FIXME (maybe): using the WriteHandle interface leads to having to create
// another copy of the readout buffer (the first copy is done in
// mvlc::ReadoutWorker::Private::flushCurrentOutputBuffer()).
class BlockingBufferQueuesWriteHandle: public mvlc::listfile::WriteHandle
{
    public:
        BlockingBufferQueuesWriteHandle(
            mvlc::ReadoutBufferQueues &destQueues,
            mvlc::ConnectionType connectionType
            )
            : destQueues_(destQueues)
            , connectionType_(connectionType)
        {
        }

        size_t write(const u8 *data, size_t size) override
        {
            auto destBuffer = destQueues_.emptyBufferQueue().dequeue_blocking();
            destBuffer->clear();
            destBuffer->setBufferNumber(nextBufferNumber_++);
            destBuffer->setType(connectionType_);
            destBuffer->ensureFreeSpace(size);
            assert(destBuffer->used() == 0);
            std::memcpy(destBuffer->data(), data, size);
            destBuffer->use(size);
            destQueues_.filledBufferQueue().enqueue(destBuffer);
            return size;
        }

    private:
        mvlc::ReadoutBufferQueues &destQueues_;
        mvlc::ConnectionType connectionType_;
        u32 nextBufferNumber_ = 1u;
};

struct EventBuilderOutputBufferWriter
{
    /* TODO:
     *
     * - find a better name for the struct
     * - if writing a listfile using the output of the EventBuilder
     *   vmeconfig and beginrun/endrun sections are needed.
     * - forced flush due to timeout and at the end of a run
     */


    using ModuleData = mvlc::readout_parser::ModuleData;

    EventBuilderOutputBufferWriter(mvlc::ReadoutBufferQueues &snoopQueues)
        : snoopQueues_(snoopQueues)
    {
    }

    ~EventBuilderOutputBufferWriter()
    {
        flushCurrentOutputBuffer();

        if (outputBuffer_)
        {
            snoopQueues_.emptyBufferQueue().enqueue(outputBuffer_);
            outputBuffer_ = nullptr;
        }
    }

    void eventData(int ci, int ei, const ModuleData *moduleDataList, unsigned moduleCount)
    {
        if (auto dest = getOutputBuffer())
        {
            mvlc::listfile::write_module_data(
                *dest, ci, ei, moduleDataList, moduleCount);
            maybeFlushOutputBuffer();
        }
        // else: data is dropped
    }

    void systemEvent(int ci, const u32 *header, u32 size)
    {
        if (auto dest = getOutputBuffer())
        {
            mvlc::listfile::write_system_event(
                *dest, ci, header, size);
            maybeFlushOutputBuffer();
        }
        // else: data is dropped
    }

    mvlc::ReadoutBuffer *getOutputBuffer()
    {
        if (!outputBuffer_)
        {
            outputBuffer_ = snoopQueues_.emptyBufferQueue().dequeue();

            if (outputBuffer_)
            {
                outputBuffer_->clear();
                outputBuffer_->setBufferNumber(nextOutputBufferNumber_++);
                outputBuffer_->setType(mvlc::ConnectionType::USB);
            }
        }

        return outputBuffer_;
    }

    void flushCurrentOutputBuffer()
    {
        if (outputBuffer_ && outputBuffer_->used() > 0)
        {
            snoopQueues_.filledBufferQueue().enqueue(outputBuffer_);
            outputBuffer_ = nullptr;
            lastFlushTime_ = std::chrono::steady_clock::now();
        }
    }

    void maybeFlushOutputBuffer()
    {
        if (!outputBuffer_ || outputBuffer_->empty())
            return;

        if (std::chrono::steady_clock::now() - lastFlushTime_ >= FlushBufferInterval)
            flushCurrentOutputBuffer();
    }

    mvlc::ReadoutBufferQueues &snoopQueues_;
    mvlc::ReadoutBuffer *outputBuffer_ = nullptr;
    u32 nextOutputBufferNumber_ = 1u;
    std::chrono::steady_clock::time_point lastFlushTime_ = std::chrono::steady_clock::now();;
    const std::chrono::milliseconds FlushBufferInterval = std::chrono::milliseconds(500);
};



// Chain to build:
// mvlc::ReadoutWorker -> listfile::WriteHandle
//   -> [write  single crate listfile to disk]
//   -> Queue -> ReadoutParser [-> MultiEventSplitter ] -> EventBuilder::recordEventData()
//   -> EventBuilder -> MergedQueue
//   -> MergedQueue -> Analysis
//                  -> MergedListfile
//
// Can leave out the merged queue in the fist iteration and directly call into
// the analysis but this makes EventBuilder and analysis run in the same
// thread.

// Threads, ReadoutWorker, ReadoutParser, EventBuilder, Analysis

struct CrateReadout
{
    using ProtectedParserCounters = mvlc::Protected<mvlc::readout_parser::ReadoutParserCounters>;

    std::unique_ptr<mvme_mvlc::MVLC_VMEController> mvlcController;
    mvlc::MVLC mvlc;
    // unused but required for mvlc::ReadoutWorker instances.
    std::unique_ptr<mvlc::ReadoutBufferQueues> readoutSnoopQueues;
    std::unique_ptr<mvlc::ReadoutWorker> readoutWorker;

    // Buffer queues between the readout worker and the readout parser. Filled
    // by an instance of BlockingBufferQueuesWriteHandle, emptied by the
    // readout parser.
    std::unique_ptr<mvlc::ReadoutBufferQueues> readoutBufferQueues;
    std::unique_ptr<BlockingBufferQueuesWriteHandle> readoutWriteHandle;

    mvlc::readout_parser::ReadoutParserState parserState;
    std::unique_ptr<ProtectedParserCounters> parserCounters;
    mvlc::readout_parser::ReadoutParserCallbacks parserCallbacks;
    std::unique_ptr<std::atomic<bool>> parserQuit;
    std::thread parserThread;

    //CrateReadout() {}
    //CrateReadout(CrateReadout &&) = default;
    //CrateReadout(const CrateReadout &) = delete;
};

struct MultiCrateReadout
{

    std::vector<CrateReadout> crateReadouts;

    std::unique_ptr<mvlc::EventBuilder> eventBuilder;
    mvlc::readout_parser::ReadoutParserCallbacks eventBuilderCallbacks;
    std::unique_ptr<std::atomic<bool>> eventBuilderQuit;
    std::thread eventBuilderThread;

    std::unique_ptr<mvlc::ReadoutBufferQueues> eventBuilderSnoopOutputQueues;
    //std::unique_ptr<mvlc::ReadoutBufferQueues> postProcessedListfileQueues;
};

} // end namespace multi_crate

#endif /* __MVME_MULTI_CRATE_H__ */
