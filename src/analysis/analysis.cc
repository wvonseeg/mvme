/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2018 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "analysis/analysis.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <random>
#include <zstr/src/zstr.hpp>

#include "analysis/a2_adapter.h"
#include "analysis/a2/multiword_datafilter.h"
#include "analysis/analysis_serialization.h"
#include "analysis/analysis_util.h"
#include "analysis/exportsink_codegen.h"
#include "analysis/object_visitor.h"
#include "vme_config.h"

#define ENABLE_ANALYSIS_DEBUG 0

template<typename T>
QDebug &operator<< (QDebug &dbg, const std::shared_ptr<T> &ptr)
{
    dbg << ptr.get();
    return dbg;
}

template<>
const QMap<analysis::Analysis::ReadResultCodes, const char *>
analysis::Analysis::ReadResult::ErrorCodeStrings =
{
    { analysis::Analysis::NoError, "No Error" },
    { analysis::Analysis::VersionTooNew, "Version too new" },
};

namespace
{
using ArenaPtr = std::unique_ptr<memory::Arena>;

using analysis::A2AdapterState;
using analysis::Analysis;

/* Performs a2_adapter_build() and prints out arena memory stats. */
A2AdapterState a2_adapter_build_memory_wrapper(
    ArenaPtr &arena,
    ArenaPtr &workArena,
    Analysis *analysis,
    const analysis::SourceVector &sources,
    const analysis::OperatorVector &operators,
    const vme_analysis_common::VMEIdToIndex &vmeMap,
    const RunInfo &runInfo,
    analysis::Logger logger = {})
{
    auto result = a2_adapter_build(
        arena.get(),
        workArena.get(),
        analysis,
        sources,
        operators,
        vmeMap,
        runInfo);

    qDebug("%s a2: mem=%u sz=%u segments=%u",
           __FUNCTION__, (u32)arena->used(), (u32)arena->size(), (u32)arena->segmentCount());

    return result;
}

QJsonObject to_json(const a2::data_filter::DataFilter &filter)
{
    QJsonObject result;

    result["filterString"] = QString::fromStdString(to_string(filter));
    result["wordIndex"] = filter.matchWordIndex;

    return result;
}

a2::data_filter::DataFilter a2_dataFilter_from_json(const QJsonObject &json)
{
    return a2::data_filter::make_filter(
        json["filterString"].toString().toStdString(),
        json["wordIndex"].toInt());
}

QJsonObject to_json(const a2::data_filter::MultiWordFilter &filter)
{
    QJsonObject result;

    QJsonArray subFilterArray;

    for (s32 i = 0; i < filter.filterCount; i++)
    {
        const auto &subfilter = filter.filters[i];
        QJsonObject filterJson;
        filterJson["filterString"] = QString::fromStdString(to_string(subfilter));
        filterJson["wordIndex"] = subfilter.matchWordIndex;
        subFilterArray.append(filterJson);
    }

    result["subFilters"] = subFilterArray;


    return result;
}

a2::data_filter::MultiWordFilter a2_multiWordFilter_from_json(const QJsonObject &json)
{
    a2::data_filter::MultiWordFilter result = {};

    auto subFilterArray = json["subFilters"].toArray();

    for (auto it = subFilterArray.begin();
         it != subFilterArray.end();
         it++)
    {
        add_subfilter(&result, a2_dataFilter_from_json(it->toObject()));
    }

    return result;
}

QJsonObject to_json(const a2::data_filter::ListFilter &filter)
{
    QJsonObject result;

    result["extractionFilter"] = to_json(filter.extractionFilter);
    result["flags"] = static_cast<qint64>(filter.flags);
    result["wordCount"] = static_cast<qint64>(filter.wordCount);

    return result;
}

a2::data_filter::ListFilter a2_listfilter_from_json(const QJsonObject &json)
{
    using a2::data_filter::ListFilter;

    ListFilter result = {};

    result.extractionFilter = a2_multiWordFilter_from_json(json["extractionFilter"].toObject());
    result.flags = static_cast<ListFilter::Flag>(json["flags"].toInt());
    result.wordCount = static_cast<u8>(json["wordCount"].toInt());

    return result;
}

} // end anon namespace

namespace analysis
{
QString to_string(const ObjectFlags::Flags &flags)
{
    QString result;

    if (flags & ObjectFlags::NeedsRebuild)
        result += "NeedsRebuild";

    return result;
}

/* File versioning. If the format changes this version needs to be incremented and a
 * conversion routine has to be implemented (analysis_serialization.cc).
 * Incrementing can also be done to force users to use a newer version of mvme to load the
 * analysis. This way they won't run into missing features/undefined behaviour.
 */
static const int CurrentAnalysisVersion = 3;

//
// AnalysisObject
//

void AnalysisObject::setUserLevel(s32 level)
{
    m_userLevel = std::max(0, level);
}

std::unique_ptr<AnalysisObject> AnalysisObject::clone() const
{
    auto qobjectPtr  = metaObject()->newInstance();
    qDebug() << metaObject()->className() << qobjectPtr;
    auto downcastPtr = qobject_cast<AnalysisObject *>(qobjectPtr);
    assert(downcastPtr);

    std::unique_ptr<AnalysisObject> result(downcastPtr);

    // Use the JSON serialization layer to clone object data.
    {
        QJsonObject tmpStorage;
        this->write(tmpStorage);
        result->read(tmpStorage);
    }

    result->setObjectName(this->objectName());
    result->setUserLevel(this->getUserLevel());
    result->setEventId(this->getEventId());
    result->postClone(this); // Let subclasses pull additional information from 'this'.

    return result;
}

//
// Pipe
//

Pipe::Pipe()
{}

Pipe::Pipe(PipeSourceInterface *sourceObject, s32 outputIndex, const QString &paramVectorName)
    : source(sourceObject)
    , sourceOutputIndex(outputIndex)
{
    setParameterName(paramVectorName);
}

void Pipe::disconnectAllDestinationSlots()
{
    for (auto slot: destinations)
    {
        slot->disconnectPipe();
    }
    destinations.clear();
}

//
// Slot
//

void Slot::connectPipe(Pipe *newInput, s32 newParamIndex)
{
    disconnectPipe();

    if (newInput)
    {
        // If the input is an array of size 1 this turns the connection into a
        // "value" connection using index 0.
        // Basically NoParamIndex is rewritten to 0 in case the input is of size 1
        // and it's ok to use Input::Value.
        if (newInput->getSize() == 1
            && acceptedInputTypes & InputType::Value)
        {
            newParamIndex = 0;
        }

        inputPipe = newInput;
        paramIndex = newParamIndex;
        inputPipe->addDestination(this);
        Q_ASSERT(parentOperator);
        parentOperator->slotConnected(this);
    }
}

void Slot::disconnectPipe()
{
    if (inputPipe)
    {
        inputPipe->removeDestination(this);
        inputPipe = nullptr;
        paramIndex = Slot::NoParamIndex;

        if (parentOperator)
        {
            parentOperator->slotDisconnected(this);
        }
    }
}

//
// SourceInterface
//
void SourceInterface::postClone(const AnalysisObject *cloneSource)
{
    auto si = qobject_cast<const SourceInterface *>(cloneSource);
    assert(si);
    this->setModuleId(si->getModuleId());
    AnalysisObject::postClone(cloneSource);
}

void SourceInterface::accept(ObjectVisitor &visitor)
{
    visitor.visit(this);
}

//
// OperatorInterface
//

QVector<Slot *> OperatorInterface::getSlots()
{
    QVector<Slot *> result;

    result.reserve(getNumberOfSlots());

    for (auto si = 0; si < getNumberOfSlots(); si++)
    {
        result.push_back(getSlot(si));
    }

    return result;
}

// FIXME: does not perform acceptedInputTypes validity test atm!
void OperatorInterface::connectInputSlot(s32 slotIndex, Pipe *inputPipe, s32 paramIndex)
{
    Slot *slot = getSlot(slotIndex);

    if (slot)
    {
        slot->connectPipe(inputPipe, paramIndex);
    }
}

s32 OperatorInterface::getMaximumInputRank()
{
    s32 result = 0;

    for (s32 slotIndex = 0;
         slotIndex < getNumberOfSlots();
         ++slotIndex)
    {
        if (Slot *slot = getSlot(slotIndex))
        {
            if (slot->inputPipe)
                result = std::max(result, slot->inputPipe->getRank());
        }
    }

    return result;
}

s32 OperatorInterface::getMaximumOutputRank()
{
    s32 result = 0;

    for (s32 outputIndex = 0;
         outputIndex < getNumberOfOutputs();
         ++outputIndex)
    {
        if (Pipe *output = getOutput(outputIndex))
        {
            result = std::max(result, output->getRank());
        }
    }

    return result;
}

void OperatorInterface::accept(ObjectVisitor &visitor)
{
    visitor.visit(this);
}

//
// SinkInterface
//

void SinkInterface::accept(ObjectVisitor &visitor)
{
    visitor.visit(this);
}

void SinkInterface::postClone(const AnalysisObject *cloneSource)
{
    auto si = qobject_cast<const SinkInterface *>(cloneSource);
    assert(si);
    this->setEnabled(si->isEnabled());
    OperatorInterface::postClone(cloneSource);
}

//
// Directory
//

QString to_string(const DisplayLocation &loc)
{
    switch (loc)
    {
        case DisplayLocation::Any:
            return QSL("any");

        case DisplayLocation::Operator:
            return QSL("operator");

        case DisplayLocation::Sink:
            return QSL("sink");
    }

    return QSL("any");
}

DisplayLocation displayLocation_from_string(const QString &str_)
{
    auto str = str_.toLower();

    if (str == QSL("operator"))
        return DisplayLocation::Operator;

    if (str == QSL("sink"))
        return DisplayLocation::Sink;

    return DisplayLocation::Any;
}

Directory::Directory(QObject *parent)
    : AnalysisObject(parent)
{ }

Directory::MemberSet Directory::getMemberSet() const
{
    MemberSet result;

    for (const auto &id: m_members)
    {
        result.insert(id);
    }

    return result;
}

void Directory::read(const QJsonObject &json)
{
    m_members.clear();

    auto memberIds = json["members"].toArray();

    for (auto it = memberIds.begin(); it != memberIds.end(); it++)
    {
        m_members.push_back(QUuid(it->toString()));
    }

    setDisplayLocation(displayLocation_from_string(json["displayLocation"].toString()));
}

void Directory::write(QJsonObject &json) const
{
    QJsonArray memberIds;

    for (const auto &id: m_members)
    {
        memberIds.append(id.toString());
    }

    json["members"] = memberIds;
    json["displayLocation"] = to_string(getDisplayLocation());
}

void Directory::postClone(const AnalysisObject *cloneSource)
{
    m_members.clear();
    AnalysisObject::postClone(cloneSource);
}

void Directory::accept(ObjectVisitor &visitor)
{
    visitor.visit(this);
}

bool check_directory_consistency(const DirectoryVector &dirs, const Analysis *analysis)
{
#ifndef QT_NO_DEBUG
    qDebug() << __PRETTY_FUNCTION__;

    QHash<DirectoryPtr, Directory::MemberSet> memberSets;

    for (const auto &dir: dirs)
    {
        memberSets.insert(dir, dir->getMemberSet());
    }

    Directory::MemberSet allMembers;

    for (auto it = memberSets.begin();
         it != memberSets.end();
         it++)
    {
        auto &dir = it.key();
        auto &set = it.value();

        if (allMembers.intersects(set))
        {
            auto duplicates = allMembers;
            duplicates.intersect(set);

            qDebug() << __PRETTY_FUNCTION__
                << "checking dir" << dir << dir->getId() << dir->objectName()
                << ", duplicates:" << duplicates;

            if (analysis)
            {
                for (const auto &did: duplicates)
                {
                    if (auto dup = analysis->getObject(did))
                    {
                        qDebug() << "  duplicate:" << dup->objectName() << dup->getId();
                    }
                }
            }

            assert(false);
        }

        allMembers.unite(set);
    }
#endif

    return true;
}


//
// Extractor
//

static std::uniform_real_distribution<double> RealDist01(0.0, 1.0);

/* This random device is used to generate the initial seeds for data extractors
 * (Extractor, ListFilterExtractor). It is _not_ used for random number
 * generation during analysis runtime, pcg32_fast is used for that. */
static std::random_device StaticRandomDevice;

Extractor::Extractor(QObject *parent)
    : SourceInterface(parent)
    , m_options(Options::NoOption)
{
    m_output.setSource(this);

    // Generate a random seed for the rng. This seed will be written out in
    // write() and restored in read().
    std::uniform_int_distribution<u64> dist;
    m_rngSeed = dist(StaticRandomDevice);
}

void Extractor::beginRun(const RunInfo &runInfo, Logger logger)
{
    m_fastFilter = {};
    for (auto slowFilter: m_filter.getSubFilters())
    {
        auto subfilter = a2::data_filter::make_filter(slowFilter.getFilter().toStdString(),
                                                      slowFilter.getWordIndex());
        add_subfilter(&m_fastFilter, subfilter);
    }

    u32 addressCount = 1u << get_extract_bits(&m_fastFilter, a2::data_filter::MultiWordFilter::CacheA);

    qDebug() << __PRETTY_FUNCTION__ << this << "addressCount" << addressCount;

    // The highest value the filter will yield is ((2^bits) - 1) but we're
    // adding a random in [0.0, 1.0) so the actual exclusive upper limit is
    // (2^bits).

    double upperLimit = std::pow(2.0, m_filter.getDataBits());

    auto &params(m_output.getParameters());
    params.resize(addressCount);

    for (s32 i=0; i<params.size(); ++i)
    {
        auto &param(params[i]);
        param.lowerLimit = 0.0;
        param.upperLimit = upperLimit;
    }

    params.name = this->objectName();
}

s32 Extractor::getNumberOfOutputs() const
{
    return 1;
}

QString Extractor::getOutputName(s32 outputIndex) const
{
    return QSL("Extracted data array");
}

Pipe *Extractor::getOutput(s32 index)
{
    Pipe *result = nullptr;

    if (index == 0)
    {
        result = &m_output;
    }

    return result;
}

void Extractor::read(const QJsonObject &json)
{
    // If a seed was stored reuse it, otherwise stick with the one generated in
    // the constructor.
    if (json.contains("rngSeed"))
    {
        // Need the full 64-bits which QJsonValue::toInt() does not provide.
        // Storing as double will lead to loss of precision which effectively
        // truncates the seed.
        // Instead the seed is stored as a string and then parsed back to u64.
        QString sSeed = json["rngSeed"].toString();
        m_rngSeed = sSeed.toULongLong(nullptr, 16);

        // convert back and compare the strings
        Q_ASSERT(sSeed == QString::number(m_rngSeed, 16));
    }

    m_filter = MultiWordDataFilter();

    auto filterArray = json["subFilters"].toArray();

    for (auto it=filterArray.begin();
         it != filterArray.end();
         ++it)
    {
        auto filterJson = it->toObject();
        auto filterString = filterJson["filterString"].toString().toLocal8Bit();
        auto wordIndex    = filterJson["wordIndex"].toInt(-1);
        DataFilter filter = makeFilterFromBytes(filterString, wordIndex);
        m_filter.addSubFilter(filter);
    }

    setRequiredCompletionCount(static_cast<u32>(json["requiredCompletionCount"].toInt()));
    m_options = static_cast<Options::opt_t>(json["options"].toInt());
}

void Extractor::write(QJsonObject &json) const
{
    json["rngSeed"] = QString::number(m_rngSeed, 16);

    QJsonArray filterArray;
    const auto &subFilters(m_filter.getSubFilters());
    for (const auto &dataFilter: subFilters)
    {
        QJsonObject filterJson;
        filterJson["filterString"] = QString::fromLocal8Bit(dataFilter.getFilter());
        filterJson["wordIndex"] = dataFilter.getWordIndex();
        filterArray.append(filterJson);
    }

    json["subFilters"] = filterArray;
    json["requiredCompletionCount"] = static_cast<qint64>(m_requiredCompletionCount);
    json["options"] = static_cast<s32>(m_options);
}

void Extractor::postClone(const AnalysisObject *cloneSource)
{
    // Generate a new seed for the clone
    std::uniform_int_distribution<u64> dist;
    m_rngSeed = dist(StaticRandomDevice);
    SourceInterface::postClone(cloneSource);
}

//
// ListFilterExtractor
//
ListFilterExtractor::ListFilterExtractor(QObject *parent)
    : SourceInterface(parent)
{
    m_output.setSource(this);
    m_a2Extractor = {};
    m_a2Extractor.options = a2::DataSourceOptions::NoAddedRandom;

    // Generate a random seed for the rng. This seed will be written out in
    // write() and restored in read().
    std::uniform_int_distribution<u64> dist;
    m_rngSeed = dist(StaticRandomDevice);
}

void ListFilterExtractor::beginRun(const RunInfo &runInfo, Logger logger)
{
    u32 addressCount = get_address_count(&m_a2Extractor);
    u32 dataBits = get_extract_bits(&m_a2Extractor.listFilter, a2::data_filter::MultiWordFilter::CacheD);
    double upperLimit = std::pow(2.0, dataBits);

    auto &params(m_output.getParameters());
    params.resize(addressCount);

    for (s32 i=0; i<params.size(); ++i)
    {
        auto &param(params[i]);
        param.lowerLimit = 0.0;
        param.upperLimit = upperLimit;
    }

    params.name = this->objectName();
}

void ListFilterExtractor::write(QJsonObject &json) const
{
    json["listFilter"] = to_json(m_a2Extractor.listFilter);
    json["repetitions"] = static_cast<qint64>(m_a2Extractor.repetitions);
    json["rngSeed"] = QString::number(m_rngSeed, 16);
    json["options"] = static_cast<s32>(m_a2Extractor.options);
}

void ListFilterExtractor::read(const QJsonObject &json)
{
    m_a2Extractor = {};
    m_a2Extractor.listFilter = a2_listfilter_from_json(json["listFilter"].toObject());
    m_a2Extractor.repetitions = static_cast<u8>(json["repetitions"].toInt());
    QString sSeed = json["rngSeed"].toString();
    m_rngSeed = sSeed.toULongLong(nullptr, 16);
    m_a2Extractor.options = static_cast<Options::opt_t>(json["options"].toInt());
}

void ListFilterExtractor::postClone(const AnalysisObject *cloneSource)
{
    // Generate a new seed for the clone
    std::uniform_int_distribution<u64> dist;
    m_rngSeed = dist(StaticRandomDevice);
    SourceInterface::postClone(cloneSource);
}

//
// BasicOperator
//
BasicOperator::BasicOperator(QObject *parent)
    : OperatorInterface(parent)
    , m_inputSlot(this, 0, QSL("Input"))
{
    m_output.setSource(this);
}

BasicOperator::~BasicOperator()
{
}

s32 BasicOperator::getNumberOfSlots() const
{
    return 1;
}

Slot *BasicOperator::getSlot(s32 slotIndex)
{
    Slot *result = nullptr;
    if (slotIndex == 0)
    {
        result = &m_inputSlot;
    }
    return result;
}

s32 BasicOperator::getNumberOfOutputs() const
{
    return 1;
}

QString BasicOperator::getOutputName(s32 outputIndex) const
{
    if (outputIndex == 0)
    {
        return QSL("Output");
    }
    return QString();

}

Pipe *BasicOperator::getOutput(s32 index)
{
    Pipe *result = nullptr;
    if (index == 0)
    {
        result = &m_output;
    }

    return result;
}

//
// BasicSink
//
BasicSink::BasicSink(QObject *parent)
    : SinkInterface(parent)
    , m_inputSlot(this, 0, QSL("Input"))
{
}

BasicSink::~BasicSink()
{
}

s32 BasicSink::getNumberOfSlots() const
{
    return 1;
}

Slot *BasicSink::getSlot(s32 slotIndex)
{
    Slot *result = nullptr;
    if (slotIndex == 0)
    {
        result = &m_inputSlot;
    }
    return result;
}

//
// CalibrationMinMax
//
CalibrationMinMax::CalibrationMinMax(QObject *parent)
    : BasicOperator(parent)
{
}

void CalibrationMinMax::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());

    out.name = objectName();
    out.unit = getUnitLabel();

    if (m_inputSlot.inputPipe)
    {
        const auto &in(m_inputSlot.inputPipe->getParameters());

        s32 idxMin = 0;
        s32 idxMax = in.size();

        if (m_inputSlot.paramIndex != Slot::NoParamIndex)
        {
            out.resize(1);
            idxMin = m_inputSlot.paramIndex;
            idxMax = idxMin + 1;
        }
        else
        {
            out.resize(in.size());
        }

        // shrink
        if (m_calibrations.size() > out.size())
        {
            m_calibrations.resize(out.size());
        }

        CalibrationMinMaxParameters firstCalib; // defaults to (nan, nan)

        if (!m_calibrations.isEmpty())
        {
            firstCalib = m_calibrations[0];
        }

        const CalibrationMinMaxParameters oldGlobalCalib{m_oldGlobalUnitMin, m_oldGlobalUnitMax};

        s32 outIdx = 0;
        for (s32 idx = idxMin; idx < idxMax; ++idx)
        {
            Parameter &outParam(out[outIdx++]);

            // Hack to make things compatible with old configs. This forces
            // expanding the calibrations array if it is too small. This
            // way it will be written to file the next time the config is
            // saved.
            if (idx >= m_calibrations.size())
            {
                if (oldGlobalCalib.isValid())
                    setCalibration(idx, oldGlobalCalib);
                else
                    setCalibration(idx, firstCalib);
            }

            // assign output limits
            auto calib = getCalibration(idx);

            outParam.lowerLimit = calib.unitMin;
            outParam.upperLimit = calib.unitMax;
        }
    }
    else
    {
        out.resize(0);
    }
}

void CalibrationMinMax::setCalibration(s32 address, const CalibrationMinMaxParameters &params)
{
    m_calibrations.resize(std::max(m_calibrations.size(), address+1));
    m_calibrations[address] = params;
}

CalibrationMinMaxParameters CalibrationMinMax::getCalibration(s32 address) const
{
    if (address < m_calibrations.size())
        return m_calibrations.at(address);

    // Support loading of legacy configs which had a calibrations array of zero
    // length but the global values set in case of inputs with size 1.
    return { m_oldGlobalUnitMin, m_oldGlobalUnitMax };
}

void CalibrationMinMax::read(const QJsonObject &json)
{
    m_unit = json["unitLabel"].toString();

    // Read the old global calbration and use it if an element of the
    // calibration array is invalid.
    // Note: these values are not written out anymore.
    m_oldGlobalUnitMin = json["globalUnitMin"].toDouble(make_quiet_nan());
    m_oldGlobalUnitMax = json["globalUnitMax"].toDouble(make_quiet_nan());

    m_calibrations.clear();
    QJsonArray calibArray = json["calibrations"].toArray();

    for (auto it=calibArray.begin();
         it != calibArray.end();
         ++it)
    {
        auto paramJson = it->toObject();


        /* TODO: There's a bug in the write code and/or the code that should
         * resize m_calibrations on changes to the input: Empty entries appear
         * at the end of the list of calibration parameters inside the
         * generated json. The test here skips those. */
        if (paramJson.contains("unitMin"))
        {
            CalibrationMinMaxParameters param;

            param.unitMin = paramJson["unitMin"].toDouble(make_quiet_nan());
            param.unitMax = paramJson["unitMax"].toDouble(make_quiet_nan());

            if (!param.isValid())
            {
                param.unitMin = m_oldGlobalUnitMin;
                param.unitMax = m_oldGlobalUnitMax;
            }

            m_calibrations.push_back(param);
        }
    }
}

void CalibrationMinMax::write(QJsonObject &json) const
{
    json["unitLabel"] = m_unit;

    QJsonArray calibArray;

    for (auto &param: m_calibrations)
    {
        QJsonObject paramJson;
        if (param.isValid())
        {
            paramJson["unitMin"] = param.unitMin;
            paramJson["unitMax"] = param.unitMax;
        }
        calibArray.append(paramJson);
    }

    json["calibrations"] = calibArray;
}
//
// IndexSelector
//
IndexSelector::IndexSelector(QObject *parent)
    : BasicOperator(parent)
{
    m_inputSlot.acceptedInputTypes = InputType::Array;
}

void IndexSelector::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());


    if (m_inputSlot.inputPipe)
    {
        const auto &in(m_inputSlot.inputPipe->getParameters());

        out.resize(1);
        out.name = objectName();// in.name;
        out.unit = in.unit;
    }
    else
    {
        out.resize(0);
        out.name = QString();
        out.unit = QString();
    }
}

void IndexSelector::read(const QJsonObject &json)
{
    m_index = json["index"].toInt();
}

void IndexSelector::write(QJsonObject &json) const
{
    json["index"] = m_index;
}

//
// PreviousValue
//
PreviousValue::PreviousValue(QObject *parent)
    : BasicOperator(parent)
{
    m_inputSlot.acceptedInputTypes = InputType::Both;
}

void PreviousValue::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());

    if (m_inputSlot.inputPipe)
    {
        const auto &in(m_inputSlot.inputPipe->getParameters());

        s32 idxMin = 0;
        s32 idxMax = in.size();

        if (m_inputSlot.paramIndex != Slot::NoParamIndex)
        {
            idxMin = m_inputSlot.paramIndex;
            idxMax = idxMin + 1;
        }

        out.resize(idxMax - idxMin);
        out.invalidateAll();
        out.name = objectName();// in.name;
        out.unit = in.unit;

        if (idxMin >= in.size())
        {
            return;
        }

        m_previousInput.resize(idxMax - idxMin);
        m_previousInput.invalidateAll();

        for (s32 idx = idxMin, outIdx = 0;
             idx < idxMax;
             ++idx, ++outIdx)
        {
            const Parameter &inParam(in[idx]);
            Parameter &outParam(out[outIdx]);

            outParam.lowerLimit = inParam.lowerLimit;
            outParam.upperLimit = inParam.upperLimit;
        }
    }
    else
    {
        out.resize(0);
        out.name = QString();
        out.unit = QString();
    }
}

void PreviousValue::read(const QJsonObject &json)
{
    m_keepValid = json["keepValid"].toBool();
}

void PreviousValue::write(QJsonObject &json) const
{
    json["keepValid"] = m_keepValid;
}

//
// RetainValid
//
RetainValid::RetainValid(QObject *parent)
    : BasicOperator(parent)
{
    m_inputSlot.acceptedInputTypes = InputType::Both;
}

void RetainValid::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());

    if (m_inputSlot.inputPipe)
    {
        const auto &in(m_inputSlot.inputPipe->getParameters());

        s32 idxMin = 0;
        s32 idxMax = in.size();

        if (m_inputSlot.paramIndex != Slot::NoParamIndex)
        {
            out.resize(1);
            idxMin = m_inputSlot.paramIndex;
            idxMax = idxMin + 1;
        }
        else
        {
            out.resize(in.size());
        }

        out.invalidateAll();
        out.name = objectName();// in.name;
        out.unit = in.unit;

        for (s32 idx = idxMin, outIdx = 0;
             idx < idxMax;
             ++idx, ++outIdx)
        {
            const Parameter &inParam(in[idx]);
            Parameter &outParam(out[outIdx]);

            outParam.lowerLimit = inParam.lowerLimit;
            outParam.upperLimit = inParam.upperLimit;
        }
    }
    else
    {
        out.resize(0);
        out.name = QString();
        out.unit = QString();
    }
}

void RetainValid::read(const QJsonObject &json)
{
}

void RetainValid::write(QJsonObject &json) const
{
}

//
// Difference
//
Difference::Difference(QObject *parent)
    : OperatorInterface(parent)
    , m_inputA(this, 0, QSL(" A"))
    , m_inputB(this, 1, QSL("-B"))
{
    m_output.setSource(this);
}

void Difference::slotConnected(Slot *slot)
{
    Q_ASSERT(slot == &m_inputA || slot == &m_inputB);

    if (slot->paramIndex != Slot::NoParamIndex)
    {
        m_inputA.acceptedInputTypes = InputType::Value;
        m_inputB.acceptedInputTypes = InputType::Value;
    }
    else
    {
        m_inputA.acceptedInputTypes = InputType::Array;
        m_inputB.acceptedInputTypes = InputType::Array;
    }
}

void Difference::slotDisconnected(Slot *slot)
{
    Q_ASSERT(slot == &m_inputA || slot == &m_inputB);

    if (!m_inputA.isConnected() && !m_inputB.isConnected())
    {
        m_inputA.acceptedInputTypes = InputType::Both;
        m_inputB.acceptedInputTypes = InputType::Both;
    }
}

void Difference::beginRun(const RunInfo &, Logger logger)
{
    m_output.parameters.name = objectName(); // QSL("A-B");
    m_output.parameters.unit = QString();

    if (!m_inputA.isParamIndexInRange() || !m_inputB.isParamIndexInRange())
    {
        m_output.parameters.resize(0);
        return;
    }

    // Either both inputs are arrays or both are single values
    Q_ASSERT((m_inputA.paramIndex == Slot::NoParamIndex && m_inputB.paramIndex == Slot::NoParamIndex)
             || (m_inputA.paramIndex != Slot::NoParamIndex && m_inputB.paramIndex != Slot::NoParamIndex));

    m_output.parameters.unit = m_inputA.inputPipe->parameters.unit;

    if (m_inputA.paramIndex != Slot::NoParamIndex && m_inputB.paramIndex != Slot::NoParamIndex)
    {
        // Both inputs are single values
        m_output.parameters.resize(1);
        auto &out(m_output.parameters[0]);
        const auto &inA(m_inputA.inputPipe->parameters[m_inputA.paramIndex]);
        const auto &inB(m_inputB.inputPipe->parameters[m_inputB.paramIndex]);
        out.lowerLimit = inA.lowerLimit - inB.upperLimit;
        out.upperLimit = inA.upperLimit - inB.lowerLimit;
    }
    else if (m_inputA.paramIndex == Slot::NoParamIndex && m_inputB.paramIndex == Slot::NoParamIndex)
    {
        // Both inputs are arrays
        s32 minSize = std::numeric_limits<s32>::max();
        minSize = std::min(minSize, m_inputA.inputPipe->parameters.size());
        minSize = std::min(minSize, m_inputB.inputPipe->parameters.size());
        m_output.parameters.resize(minSize);
        for (s32 idx = 0; idx < minSize; ++idx)
        {
            auto &out(m_output.parameters[idx]);
            const auto &inA(m_inputA.inputPipe->parameters[idx]);
            const auto &inB(m_inputB.inputPipe->parameters[idx]);

            out.lowerLimit = inA.lowerLimit - inB.upperLimit;
            out.upperLimit = inA.upperLimit - inB.lowerLimit;
        }
    }
    else
    {
        InvalidCodePath;
    }
}

void Difference::read(const QJsonObject &json)
{
}

void Difference::write(QJsonObject &json) const
{
}

//
// Sum
//
Sum::Sum(QObject *parent)
    : BasicOperator(parent)
{
    m_inputSlot.acceptedInputTypes = InputType::Array;
}

void Sum::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());

    if (m_inputSlot.inputPipe)
    {
        out.resize(1);

        const auto &in(m_inputSlot.inputPipe->getParameters());
        out.name = objectName();// in.name;
        out.unit = in.unit;

        double lowerBound = 0.0;
        double upperBound = 0.0;

        for (s32 i = 0; i < in.size(); ++i)
        {
            const auto &param(in[i]);

            lowerBound += std::min(param.lowerLimit, param.upperLimit);
            upperBound += std::max(param.lowerLimit, param.upperLimit);
        }

        if (m_calculateMean)
        {
            lowerBound /= in.size();
            upperBound /= in.size();
        }

        out[0].lowerLimit = std::min(lowerBound, upperBound);
        out[0].upperLimit = std::max(lowerBound, upperBound);
    }
    else
    {
        out.resize(0);
        out.name = QString();
        out.unit = QString();
    }
}

void Sum::read(const QJsonObject &json)
{
    m_calculateMean = json["isMean"].toBool();
}

void Sum::write(QJsonObject &json) const
{
    json["isMean"] = m_calculateMean;
}

//
// AggregateOps
//

// FIXME: AggregateOps implementation is horrible

static QString aggregateOp_to_string(AggregateOps::Operation op)
{
    switch (op)
    {
        case AggregateOps::Op_Sum:
            return QSL("sum");
        case AggregateOps::Op_Mean:
            return QSL("mean");
        case AggregateOps::Op_Sigma:
            return QSL("sigma");
        case AggregateOps::Op_Min:
            return QSL("min");
        case AggregateOps::Op_Max:
            return QSL("max");
        case AggregateOps::Op_Multiplicity:
            return QSL("multiplicity");
        case AggregateOps::Op_MinX:
            return QSL("maxx");
        case AggregateOps::Op_MaxX:
            return QSL("maxy");
        case AggregateOps::Op_MeanX:
            return QSL("meanx");
        case AggregateOps::Op_SigmaX:
            return QSL("sigmax");

        case AggregateOps::NumOps:
            break;
    }
    return {};
}

static AggregateOps::Operation aggregateOp_from_string(const QString &str)
{
    if (str == QSL("sum"))
        return AggregateOps::Op_Sum;

    if (str == QSL("mean"))
        return AggregateOps::Op_Mean;

    if (str == QSL("sigma"))
        return AggregateOps::Op_Sigma;

    if (str == QSL("min"))
        return AggregateOps::Op_Min;

    if (str == QSL("max"))
        return AggregateOps::Op_Max;

    if (str == QSL("multiplicity"))
        return AggregateOps::Op_Multiplicity;

    if (str == QSL("maxx"))
        return AggregateOps::Op_MinX;

    if (str == QSL("maxy"))
        return AggregateOps::Op_MaxX;

    if (str == QSL("meanx"))
        return AggregateOps::Op_MeanX;

    if (str == QSL("sigmax"))
        return AggregateOps::Op_SigmaX;

    return AggregateOps::Op_Sum;
}

AggregateOps::AggregateOps(QObject *parent)
    : BasicOperator(parent)
{
    m_inputSlot.acceptedInputTypes = InputType::Array;
}

// FIXME: min and max thresholds are not taken into account when calculating
// the output lower and upper limits!
void AggregateOps::beginRun(const RunInfo &runInfo, Logger logger)
{
    auto &out(m_output.getParameters());

    if (m_inputSlot.inputPipe)
    {
        const auto &in(m_inputSlot.inputPipe->getParameters());

        out.resize(1);
        out.name = objectName();
        out.unit = m_outputUnitLabel.isEmpty() ? in.unit : m_outputUnitLabel;

        double lowerBound = 0.0;
        double upperBound = 0.0;

        switch (m_op)
        {
            case Op_Multiplicity:
                {
                    lowerBound = 0.0;
                    upperBound = in.size();
                } break;

            case Op_Sigma: // FIXME: sigma bounds
            case Op_Min:
            case Op_Max:
                {
                    double llMin = std::numeric_limits<double>::max();
                    double ulMax = std::numeric_limits<double>::lowest();

                    for (s32 i = 0; i < in.size(); ++i)
                    {
                        const auto &param(in[i]);

                        llMin = std::min(llMin, std::min(param.lowerLimit, param.upperLimit));
                        ulMax = std::max(ulMax, std::max(param.lowerLimit, param.upperLimit));
                    }

                    if (m_op == Op_Sigma)
                    {
                        lowerBound = 0.0;
                        upperBound = std::sqrt(ulMax - llMin);
                    }
                    else
                    {
                        lowerBound = llMin;
                        upperBound = ulMax;
                    }
                } break;

            case Op_Sum:
            case Op_Mean:
                {
                    for (s32 i = 0; i < in.size(); ++i)
                    {
                        const auto &param(in[i]);

                        lowerBound += std::min(param.lowerLimit, param.upperLimit);
                        upperBound += std::max(param.lowerLimit, param.upperLimit);
                    }

                    if (m_op == Op_Mean)
                    {
                        lowerBound /= in.size();
                        upperBound /= in.size();
                    }
                } break;

            case Op_MinX:
            case Op_MaxX:
            case Op_SigmaX: // FIXME: sigma bounds
            case Op_MeanX:
                {
                    lowerBound = 0.0;
                    upperBound = in.size();
                } break;

            case NumOps:
                break;
        }

        out[0].lowerLimit = std::min(lowerBound, upperBound);
        out[0].upperLimit = std::max(lowerBound, upperBound);
    }
    else
    {
        out.resize(0);
        out.name = QString();
        out.unit = QString();
    }
}

void AggregateOps::read(const QJsonObject &json)
{
    m_op = aggregateOp_from_string(json["operation"].toString());
    m_minThreshold = json["minThreshold"].toDouble(make_quiet_nan());
    m_maxThreshold = json["maxThreshold"].toDouble(make_quiet_nan());
    m_outputUnitLabel = json["outputUnitLabel"].toString();
}

void AggregateOps::write(QJsonObject &json) const
{
    json["operation"] = aggregateOp_to_string(m_op);
    json["minThreshold"] = m_minThreshold;
    json["maxThreshold"] = m_maxThreshold;
    json["outputUnitLabel"] = m_outputUnitLabel;
}

QString AggregateOps::getDisplayName() const
{
    return QSL("Aggregate Operations");
}

QString AggregateOps::getShortName() const
{
    if (m_op == AggregateOps::Op_Multiplicity)
        return QSL("Mult");

    return getOperationName(m_op);
}

void AggregateOps::setOperation(Operation op)
{
    m_op = op;
}

AggregateOps::Operation AggregateOps::getOperation() const
{
    return m_op;
}

void AggregateOps::setMinThreshold(double t)
{
    m_minThreshold = t;
}

double AggregateOps::getMinThreshold() const
{
    return m_minThreshold;
}

void AggregateOps::setMaxThreshold(double t)
{
    m_maxThreshold = t;
}

double AggregateOps::getMaxThreshold() const
{
    return m_maxThreshold;
}

QString AggregateOps::getOperationName(Operation op)
{
    switch (op)
    {
        case Op_Sum:
            return QSL("Sum");
        case Op_Mean:
            return QSL("Mean");
        case Op_Sigma:
            return QSL("Sigma");
        case Op_Min:
            return QSL("Min");
        case Op_Max:
            return QSL("Max");
        case Op_Multiplicity:
            return QSL("Multiplicity");
        case AggregateOps::Op_MinX:
            return QSL("MinX");
        case AggregateOps::Op_MaxX:
            return QSL("MaxX");
        case AggregateOps::Op_MeanX:
            return QSL("MeanX");
        case AggregateOps::Op_SigmaX:
            return QSL("SigmaX");

        case AggregateOps::NumOps:
            break;
    }
    return QString();
}

//
// ArrayMap
//
ArrayMap::ArrayMap(QObject *parent)
    : OperatorInterface(parent)
{
    addSlot();
    m_output.setSource(this);
}

bool ArrayMap::addSlot()
{
    /* If InputType::Array is passed directly inside make_shared() call I get
     * an "undefined reference to InputType::Array. */
    auto inputType = InputType::Array;

    auto slot = std::make_shared<Slot>(
        this, getNumberOfSlots(),
        QSL("Input#") + QString::number(getNumberOfSlots()), inputType);

    m_inputs.push_back(slot);

    return true;
}

bool ArrayMap::removeLastSlot()
{
    if (getNumberOfSlots() > 1)
    {
        m_inputs.back()->disconnectPipe();
        m_inputs.pop_back();

        return true;
    }

    return false;
}

void ArrayMap::beginRun(const RunInfo &, Logger logger)
{
    s32 mappingCount = m_mappings.size();
    m_output.parameters.name = objectName();
    m_output.parameters.resize(mappingCount);

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << this << "#mappings =" << mappingCount;
#endif

    for (s32 mIndex = 0;
         mIndex < mappingCount;
         ++mIndex)
    {
        IndexPair ip(m_mappings.at(mIndex));
        Parameter *inParam = nullptr;
        Slot *inputSlot = ip.slotIndex < m_inputs.size() ? m_inputs[ip.slotIndex].get() : nullptr;

        if (inputSlot && inputSlot->inputPipe)
        {
            inParam = inputSlot->inputPipe->getParameter(ip.paramIndex);

            if (mIndex == 0)
            {
                // Use the first inputs name and unit label.
                //m_output.parameters.name = inputSlot->inputPipe->parameters.name;
                m_output.parameters.unit = inputSlot->inputPipe->parameters.unit;
            }
        }

        if (inParam)
        {
            m_output.parameters[mIndex].lowerLimit = inParam->lowerLimit;
            m_output.parameters[mIndex].upperLimit = inParam->upperLimit;
        }
    }

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << this << "#output params =" << m_output.parameters.size();
#endif
}

s32 ArrayMap::getNumberOfSlots() const
{
    return m_inputs.size();
}

Slot *ArrayMap::getSlot(s32 slotIndex)
{
    Slot *result = nullptr;

    if (slotIndex < getNumberOfSlots())
    {
        result = m_inputs[slotIndex].get();
    }

    return result;
}

s32 ArrayMap::getNumberOfOutputs() const
{
    return 1;
}

QString ArrayMap::getOutputName(s32 outputIndex) const
{
    return QSL("Output");
}

Pipe *ArrayMap::getOutput(s32 index)
{
    return &m_output;
}

void ArrayMap::read(const QJsonObject &json)
{
    for (auto &slot: m_inputs)
    {
        slot->disconnectPipe();
    }
    m_inputs.clear();

    s32 inputCount = json["numberOfInputs"].toInt();

    for (s32 inputIndex = 0;
         inputIndex < inputCount;
         ++inputIndex)
    {
        addSlot();
    }

    auto mappingsArray = json["mappings"].toArray();

    for (auto it = mappingsArray.begin();
         it != mappingsArray.end();
         ++it)
    {
        auto objectJson = it->toObject();
        IndexPair ip;
        ip.slotIndex  = objectJson["slotIndex"].toInt();
        ip.paramIndex = objectJson["paramIndex"].toInt();
        m_mappings.push_back(ip);
    }
}

void ArrayMap::write(QJsonObject &json) const
{
    json["numberOfInputs"] = getNumberOfSlots();

    QJsonArray mappingsArray;

    for (auto mapping: m_mappings)
    {
        QJsonObject dest;
        dest["slotIndex"]  = mapping.slotIndex;
        dest["paramIndex"] = mapping.paramIndex;
        mappingsArray.append(dest);
    }

    json["mappings"] = mappingsArray;
}

QString ArrayMap::getDisplayName() const
{
    return QSL("Array Map");
}

QString ArrayMap::getShortName() const
{
    return QSL("Map");
}

//
// RangeFilter1D
//
RangeFilter1D::RangeFilter1D(QObject *parent)
    : BasicOperator(parent)
{
    m_inputSlot.acceptedInputTypes = InputType::Both;
}

void RangeFilter1D::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());
    out.resize(0);
    out.name = objectName();
    out.unit = QString();

    if (m_inputSlot.isParamIndexInRange())
    {
        const auto &in(m_inputSlot.inputPipe->getParameters());

        s32 idxMin = 0;
        s32 idxMax = in.size();

        if (m_inputSlot.isParameterConnection())
        {
            out.resize(1);
            idxMin = m_inputSlot.paramIndex;
            idxMax = idxMin + 1;
        }
        else
        {
            out.resize(in.size());
        }

        out.invalidateAll();
        out.unit = in.unit;

        for (s32 idx = idxMin, outIdx = 0;
             idx < idxMax;
             ++idx, ++outIdx)
        {
            auto &outParam(out[outIdx]);
            const auto &inParam(in[idx]);

            if (!m_keepOutside)
            {
                outParam.lowerLimit = m_minValue;
                outParam.upperLimit = m_maxValue;
            }
            else
            {
                outParam.lowerLimit = inParam.lowerLimit;
                outParam.upperLimit = inParam.upperLimit;
            }
        }
    }
}

void RangeFilter1D::read(const QJsonObject &json)
{
    m_minValue = json["minValue"].toDouble();
    m_maxValue = json["maxValue"].toDouble();
    m_keepOutside = json["keepOutside"].toBool();
}

void RangeFilter1D::write(QJsonObject &json) const
{
    json["minValue"] = m_minValue;
    json["maxValue"] = m_maxValue;
    json["keepOutside"] = m_keepOutside;
}

//
// ConditionFilter
//
ConditionFilter::ConditionFilter(QObject *parent)
    : OperatorInterface(parent)
    , m_dataInput(this, 0, QSL("Data"))
    , m_conditionInput(this, 1, QSL("Condition"))
    , m_invertedCondition(false)
{
    m_output.setSource(this);
    m_dataInput.acceptedInputTypes = InputType::Both;
    m_conditionInput.acceptedInputTypes = InputType::Both;
}

void ConditionFilter::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());

    out.resize(0);
    out.name = objectName();// in.name;
    out.unit = QString();

    if (!m_dataInput.isParamIndexInRange() || !m_conditionInput.isParamIndexInRange())
    {
        return;
    }

    if (m_dataInput.isConnected() && m_conditionInput.isConnected())
    {
        const auto &in(m_dataInput.inputPipe->getParameters());

        s32 idxMin = 0;
        s32 idxMax = in.size();

        if (m_dataInput.isParameterConnection())
        {
            out.resize(1);
            idxMin = m_dataInput.paramIndex;
            idxMax = idxMin + 1;
        }
        else
        {
            out.resize(in.size());
        }

        out.invalidateAll();
        //out.name = in.name;
        out.unit = in.unit;

        for (s32 idx = idxMin, outIdx = 0;
             idx < idxMax;
             ++idx, ++outIdx)
        {
            const Parameter &inParam(in[idx]);
            Parameter &outParam(out[outIdx]);

            outParam.lowerLimit = inParam.lowerLimit;
            outParam.upperLimit = inParam.upperLimit;
        }
    }
}

// Inputs
s32 ConditionFilter::getNumberOfSlots() const
{
    return 2;
}

Slot *ConditionFilter::getSlot(s32 slotIndex)
{
    if (slotIndex == 0)
    {
        return &m_dataInput;
    }
    else if (slotIndex == 1)
    {
        return &m_conditionInput;
    }

    return nullptr;
}

s32 ConditionFilter::getNumberOfOutputs() const
{
    return 1;
}

QString ConditionFilter::getOutputName(s32 outputIndex) const
{
    return QSL("Output");
}

Pipe *ConditionFilter::getOutput(s32 index)
{
    return &m_output;
}

void ConditionFilter::write(QJsonObject &json) const
{
    json["inverted"] = m_invertedCondition;
}

void ConditionFilter::read(const QJsonObject &json)
{
    m_invertedCondition = json["inverted"].toBool();
}

//
// RectFilter2D
//
RectFilter2D::RectFilter2D(QObject *parent)
    : OperatorInterface(parent)
    , m_xInput(this, 0, QSL("X Data"))
    , m_yInput(this, 1, QSL("Y Data"))
{
    m_output.setSource(this);
    m_xInput.acceptedInputTypes = InputType::Value;
    m_yInput.acceptedInputTypes = InputType::Value;
}

void RectFilter2D::beginRun(const RunInfo &, Logger logger)
{
    auto &out(m_output.getParameters());
    out.resize(0);
    out.name = objectName();
    out.unit = QString();

    if (!m_xInput.isParamIndexInRange() || !m_yInput.isParamIndexInRange())
        return;

    // Both connected and in range
    out.resize(1);
}

s32 RectFilter2D::getNumberOfSlots() const
{
    return 2;
}

Slot *RectFilter2D::getSlot(s32 slotIndex)
{
    switch (slotIndex)
    {
        case 0:
            return &m_xInput;
        case 1:
            return &m_yInput;
    }
    return nullptr;
}

s32 RectFilter2D::getNumberOfOutputs() const
{
    return 1;
}

QString RectFilter2D::getOutputName(s32 outputIndex) const
{
    return QSL("Output");
}

Pipe *RectFilter2D::getOutput(s32 index)
{
    return &m_output;
}

void RectFilter2D::read(const QJsonObject &json)
{
    m_op = OpAnd;
    if (json["operator"].toString() == "or")
    {
        m_op = OpOr;
    }

    double x1 = json["x1"].toDouble();
    double x2 = json["x2"].toDouble();
    double y1 = json["y1"].toDouble();
    double y2 = json["y2"].toDouble();

    setXInterval(x1, x2);
    setYInterval(y1, y2);
}

void RectFilter2D::write(QJsonObject &json) const
{
    json["operator"] = (m_op == OpAnd ? QSL("and") : QSL("or"));
    json["x1"] = m_xInterval.minValue();
    json["x2"] = m_xInterval.maxValue();
    json["y1"] = m_yInterval.minValue();
    json["y2"] = m_yInterval.maxValue();
}

//
// BinarySumDiff
//
struct EquationImpl
{
    const QString displayString;
    // args are (inputA, inputB, output)
    void (*impl)(const ParameterVector &, const ParameterVector &, ParameterVector &);
};

// Do not reorder the array as indexes are stored in config files!
static const QVector<EquationImpl> EquationImpls =
{
    { QSL("C = A + B"), [](const ParameterVector &a, const ParameterVector &b, ParameterVector &o)
        {
            for (s32 i = 0; i < a.size(); ++i)
            {
                o[i].valid = a[i].valid && b[i].valid;
                o[i].value = a[i].value +  b[i].value;
            }
        }
    },

    { QSL("C = A - B"), [](const ParameterVector &a, const ParameterVector &b, ParameterVector &o)
        {
            for (s32 i = 0; i < a.size(); ++i)
            {
                o[i].valid = a[i].valid && b[i].valid;
                o[i].value = a[i].value -  b[i].value;
            }
        }
    },

    { QSL("C = (A + B) / (A - B)"), [](const ParameterVector &a, const ParameterVector &b, ParameterVector &o)
        {
            for (s32 i = 0; i < a.size(); ++i)
            {
                o[i].valid = (a[i].valid && b[i].valid && (a[i].value - b[i].value != 0.0));

                if (o[i].valid)
                {
                    o[i].value = (a[i].value + b[i].value) / (a[i].value - b[i].value);
                }
            }
        }
    },

    { QSL("C = (A - B) / (A + B)"), [](const ParameterVector &a, const ParameterVector &b, ParameterVector &o)
        {
            for (s32 i = 0; i < a.size(); ++i)
            {
                o[i].valid = (a[i].valid && b[i].valid && (a[i].value + b[i].value != 0.0));

                if (o[i].valid)
                {
                    o[i].value = (a[i].value - b[i].value) / (a[i].value + b[i].value);
                }
            }
        }
    },

    { QSL("C = A / (A - B)"), [](const ParameterVector &a, const ParameterVector &b, ParameterVector &o)
        {
            for (s32 i = 0; i < a.size(); ++i)
            {
                o[i].valid = (a[i].valid && b[i].valid && (a[i].value - b[i].value != 0.0));

                if (o[i].valid)
                {
                    o[i].value = a[i].value / (a[i].value - b[i].value);
                }
            }
        }
    },

    { QSL("C = (A - B) / A"), [](const ParameterVector &a, const ParameterVector &b, ParameterVector &o)
        {
            for (s32 i = 0; i < a.size(); ++i)
            {
                o[i].valid = (a[i].valid && b[i].valid && (a[i].value != 0.0));

                if (o[i].valid)
                {
                    o[i].value = (a[i].value - b[i].value) / a[i].value;
                }
            }
        }
    },
};

BinarySumDiff::BinarySumDiff(QObject *parent)
    : OperatorInterface(parent)
    , m_inputA(this, 0, QSL("A"))
    , m_inputB(this, 1, QSL("B"))
    , m_equationIndex(0)
    , m_outputLowerLimit(0.0)
    , m_outputUpperLimit(0.0)
{
    m_output.setSource(this);
    m_inputA.acceptedInputTypes = InputType::Both;
    m_inputB.acceptedInputTypes = InputType::Both;
}

s32 BinarySumDiff::getNumberOfEquations() const
{
    return EquationImpls.size();
}

QString BinarySumDiff::getEquationDisplayString(s32 index) const
{
    if (0 <= index && index < EquationImpls.size())
    {
        return EquationImpls.at(index).displayString;
    }

    return QString();
}

void BinarySumDiff::beginRun(const RunInfo &, Logger logger)
{
    if (!(0 <= m_equationIndex && m_equationIndex < EquationImpls.size()))
    {
        return;
    }

    auto &out(m_output.getParameters());

    if (!m_inputA.isParamIndexInRange() || !m_inputB.isParamIndexInRange())
    {
        out.resize(0);
        out.name = QString();
        out.unit = QString();
        return;
    }

    // Either both inputs are arrays or both are single values
    Q_ASSERT((m_inputA.paramIndex == Slot::NoParamIndex && m_inputB.paramIndex == Slot::NoParamIndex)
             || (m_inputA.paramIndex != Slot::NoParamIndex && m_inputB.paramIndex != Slot::NoParamIndex));


    if (m_inputA.paramIndex != Slot::NoParamIndex && m_inputB.paramIndex != Slot::NoParamIndex)
    {
        // Both inputs are single values
        out.resize(1);
    }
    else if (m_inputA.paramIndex == Slot::NoParamIndex && m_inputB.paramIndex == Slot::NoParamIndex)
    {
        // Both inputs are arrays
        s32 minSize = std::min(m_inputA.inputPipe->parameters.size(),
                               m_inputB.inputPipe->parameters.size());

        out.resize(minSize);
        out.name = objectName();
        out.unit = m_outputUnitLabel;
    }
    else
    {
        out.resize(0);
    }

    out.invalidateAll();

    for (auto &param: out)
    {
        param.lowerLimit = m_outputLowerLimit;
        param.upperLimit = m_outputUpperLimit;
    }
}

s32 BinarySumDiff::getNumberOfSlots() const
{
    return 2;
}

Slot *BinarySumDiff::getSlot(s32 slotIndex)
{
    switch (slotIndex)
    {
        case 0:
            return &m_inputA;
        case 1:
            return &m_inputB;
    }
    return nullptr;
}

void BinarySumDiff::slotConnected(Slot *slot)
{
    Q_ASSERT(slot == &m_inputA || slot == &m_inputB);

    if (slot->paramIndex != Slot::NoParamIndex)
    {
        m_inputA.acceptedInputTypes = InputType::Value;
        m_inputB.acceptedInputTypes = InputType::Value;
    }
    else
    {
        m_inputA.acceptedInputTypes = InputType::Array;
        m_inputB.acceptedInputTypes = InputType::Array;
    }
}

void BinarySumDiff::slotDisconnected(Slot *slot)
{
    Q_ASSERT(slot == &m_inputA || slot == &m_inputB);

    if (!m_inputA.isConnected() && !m_inputB.isConnected())
    {
        m_inputA.acceptedInputTypes = InputType::Both;
        m_inputB.acceptedInputTypes = InputType::Both;
    }
}

s32 BinarySumDiff::getNumberOfOutputs() const
{
    return 1;
}

QString BinarySumDiff::getOutputName(s32 outputIndex) const
{
    return QSL("Output");
}

Pipe *BinarySumDiff::getOutput(s32 index)
{
    return &m_output;
}

void BinarySumDiff::read(const QJsonObject &json)
{
    m_equationIndex    = json["equationIndex"].toInt();
    m_outputUnitLabel  = json["outputUnitLabel"].toString();
    m_outputLowerLimit = json["outputLowerLimit"].toDouble();
    m_outputUpperLimit = json["outputUpperLimit"].toDouble();
}

void BinarySumDiff::write(QJsonObject &json) const
{
    json["equationIndex"]    = m_equationIndex;
    json["outputUnitLabel"]  = m_outputUnitLabel;
    json["outputLowerLimit"] = m_outputLowerLimit;
    json["outputUpperLimit"] = m_outputUpperLimit;
}

//
// ExpressionOperator
//

/* NOTES about the ExpressionOperator:

 * This is the first operator using multiple outputs and at the same time having
   a variable number of outputs. There will be bugs.

 * The number of outputs is only known once all inputs are connected and the
   begin expression has been evaluated. In Analysis::read() where the
   connections are being created the operator is not fully functional yet as
   that code doesn't sort operators by rank nor does a full build of the
   system.
   How to fix this issue and create the outputs during read() time?
   -> Store the last known number of outputs in the analysis config and create
      that many in ExpressionOperator::read()
      This means connections that where valid at the time the analyis config
      was written can be re-established when reading the config back in.
      This is now stored in 'lastOutputCount'
 */

ExpressionOperator::ExpressionOperator(QObject *parent)
    : OperatorInterface(parent)
{
    QString genericIntroComment;

    {
        QFile f(QSL(":/analysis/expr_data/generic_intro_comment.exprtk"));
        f.open(QIODevice::ReadOnly);
        genericIntroComment = QString::fromUtf8(f.readAll());
    }

    {
        QFile f(QSL(":/analysis/expr_data/basic_begin_script.exprtk"));
        f.open(QIODevice::ReadOnly);
        m_exprBegin = genericIntroComment + "\n" + QString::fromUtf8(f.readAll());
    }

    {
        QFile f(QSL(":/analysis/expr_data/basic_step_script.exprtk"));
        f.open(QIODevice::ReadOnly);
        m_exprStep = genericIntroComment + "\n" + QString::fromUtf8(f.readAll());
    }

    // Need at least one input slot to be usable
    addSlot();
}

a2::Operator ExpressionOperator::buildA2Operator(memory::Arena *arena)
{
    return buildA2Operator(arena, a2::ExpressionOperatorBuildOptions::FullBuild);
}

a2::Operator ExpressionOperator::buildA2Operator(memory::Arena *arena,
                                                 a2::ExpressionOperatorBuildOptions buildOptions)
{
    /* NOTE: This method creates "fake" a2 input pipes inside the arena. This
     * means it cannot be used inside the a1->a2 adapter layer. */

    if (!required_inputs_connected_and_valid(this))
        throw std::runtime_error("Not all required inputs are connected.");

    std::vector<a2::PipeVectors> a2_inputs;
    std::vector<s32> inputIndexes;
    std::vector<std::string> inputUnits;

    for (auto slot: m_inputs)
    {
        auto a1_pipe = slot->inputPipe;
        a2::PipeVectors a2_pipe = make_a2_pipe_from_a1_pipe(arena, a1_pipe);

        a2_inputs.push_back(a2_pipe);
        inputIndexes.push_back(slot->paramIndex);

        inputUnits.push_back(a1_pipe->parameters.unit.toStdString());
    }

    std::vector<std::string> inputPrefixes;

    for (s32 i = 0; i < m_inputs.size(); i++)
    {
        std::string inputPrefix;

        if (i < m_inputPrefixes.size())
        {
            inputPrefix = m_inputPrefixes[i].toStdString();
        }
        else
        {
            std::stringstream ss;
            ss << "input" << i;
            inputPrefix = ss.str();
        }

        inputPrefixes.push_back(inputPrefix);
    }

    assert(m_inputs.size() == static_cast<s32>(inputPrefixes.size()));

    auto a2_op = a2::make_expression_operator(
        arena,
        a2_inputs,
        inputIndexes,
        inputPrefixes,
        inputUnits,
        m_exprBegin.toStdString(),
        m_exprStep.toStdString(),
        buildOptions);

    return a2_op;
}


bool ExpressionOperator::addSlot()
{
    s32 slotCount  = getNumberOfSlots();
    auto inputType = InputType::Both;
    QString inputName;

    if (m_inputPrefixes.size() > slotCount)
    {
        inputName = m_inputPrefixes[slotCount];
    }
    else
    {
        inputName = QSL("input") + QString::number(slotCount);
        m_inputPrefixes.push_back(inputName);
    }

    auto slot = std::make_shared<Slot>(this, slotCount, inputName, inputType);

    m_inputs.push_back(slot);

    return true;
}

bool ExpressionOperator::removeLastSlot()
{
    if (getNumberOfSlots() > 1)
    {
        m_inputs.back()->disconnectPipe();
        m_inputs.pop_back();
        m_inputPrefixes.pop_back();
        assert(m_inputPrefixes.size() == m_inputs.size());
        return true;
    }
    return false;
}

s32 ExpressionOperator::getNumberOfSlots() const
{
    return m_inputs.size();
}

Slot *ExpressionOperator::getSlot(s32 slotIndex)
{
    return (slotIndex < getNumberOfSlots()
            ? m_inputs.at(slotIndex).get()
            : nullptr);
}

s32 ExpressionOperator::getNumberOfOutputs() const
{
    return m_outputs.size();
}

QString ExpressionOperator::getOutputName(s32 outputIndex) const
{
    if (auto pp = m_outputs.value(outputIndex))
    {
        return pp->getParameterName();
    }

    return {};
}

Pipe *ExpressionOperator::getOutput(s32 index)
{
    if (auto pp = m_outputs.value(index))
    {
        return pp.get();
    }
    return nullptr;
}

void ExpressionOperator::beginRun(const RunInfo &runInfo, Logger logger)
{
    try
    {
        /* Create the a2 operator which runs the begin script to figure out the
         * output size and limits. Then copy the limits to this operators output
         * pipes. */

        memory::Arena arena(Kilobytes(256));

        auto a2_op = buildA2Operator(&arena, a2::ExpressionOperatorBuildOptions::FullBuild);
        auto d     = reinterpret_cast<a2::ExpressionOperatorData *>(a2_op.d);

        assert(a2_op.outputCount == d->output_units.size());
        assert(d->output_units.size() == d->output_names.size());

        /* Disconnect Pipes that will be removed. */
        for (size_t outIdx = a2_op.outputCount;
             outIdx < static_cast<size_t>(m_outputs.size());
             outIdx++)
        {
            m_outputs[outIdx]->disconnectAllDestinationSlots();
        }

        /* Resize to the new output count. This keeps existing pipes which
         * means existing slot connections will remain valid. */
        m_outputs.resize(a2_op.outputCount);

        for (size_t outIdx = 0; outIdx < a2_op.outputCount; outIdx++)
        {
            // Reuse pipes to not invalidate existing connections
            auto outPipe = m_outputs.value(outIdx);

            if (!outPipe)
            {
                outPipe = std::make_shared<Pipe>(this, outIdx);
                m_outputs[outIdx] = outPipe;
            }

            assert(a2_op.outputs[outIdx].size == a2_op.outputLowerLimits[outIdx].size);
            assert(a2_op.outputs[outIdx].size == a2_op.outputUpperLimits[outIdx].size);

            outPipe->parameters.resize(a2_op.outputs[outIdx].size);
            outPipe->parameters.invalidateAll();
            outPipe->parameters.name = QString::fromStdString(d->output_names[outIdx]);
            outPipe->parameters.unit = QString::fromStdString(d->output_units[outIdx]);

            for (s32 paramIndex = 0; paramIndex < outPipe->parameters.size(); paramIndex++)
            {
                outPipe->parameters[paramIndex].lowerLimit = a2_op.outputLowerLimits[outIdx][paramIndex];
                outPipe->parameters[paramIndex].upperLimit = a2_op.outputUpperLimits[outIdx][paramIndex];
            }
        }
    }
    catch (const std::runtime_error &e)
    {
        if (logger)
        {
            logger(QString::fromStdString(e.what()));
        }
        qDebug() << __PRETTY_FUNCTION__ << e.what();

        /* On error keep existing pipes but resize them to zero length. This
         * way pipe -> slot connections will persist. Full array connections by
         * dependent operators will remain valid but be of size zero, indexed
         * connections will be out of range. */
        for (auto &outPipe: m_outputs)
        {
            outPipe->parameters.resize(0);
        }
    }
}

void ExpressionOperator::write(QJsonObject &json) const
{
    json["exprBegin"] = m_exprBegin;
    json["exprStep"]  = m_exprStep;

    QJsonArray inputPrefixesArray;

    for (const auto &inputName: m_inputPrefixes)
    {
        inputPrefixesArray.append(inputName);
    }

    json["inputPrefixes"] = inputPrefixesArray;
    json["lastOutputCount"] = getNumberOfOutputs();
}

void ExpressionOperator::read(const QJsonObject &json)
{
    for (auto &slot: m_inputs)
    {
        slot->disconnectPipe();
    }
    m_inputs.clear();

    m_exprBegin = json["exprBegin"].toString();
    m_exprStep  = json["exprStep"].toString();
    m_inputPrefixes.clear();

    auto inputPrefixesArray = json["inputPrefixes"].toArray();

    for (auto it = inputPrefixesArray.begin();
         it != inputPrefixesArray.end();
         it++)
    {
        m_inputPrefixes.push_back(it->toString());
        addSlot();
    }


    /* Similar to the code in beginRun(): disconnect pipes that will be
     * removed, reuse existing ones and add new ones. */

    s32 lastOutputCount = json["lastOutputCount"].toInt(0);

    for (s32 outIdx = lastOutputCount; outIdx < m_outputs.size(); outIdx++)
    {
        m_outputs[outIdx]->disconnectAllDestinationSlots();
    }

    m_outputs.resize(lastOutputCount);

    for (s32 outIdx = 0; outIdx < lastOutputCount; outIdx++)
    {
        auto outPipe = m_outputs.value(outIdx);

        if (!outPipe)
        {
            outPipe = std::make_shared<Pipe>(this, outIdx, QSL("output") + QString::number(outIdx));
            m_outputs[outIdx] = outPipe;
        }
    }
}

ExpressionOperator *ExpressionOperator::cloneViaSerialization() const
{
    QJsonObject transferData;
    this->write(transferData);

    auto result = std::make_unique<ExpressionOperator>();
    result->read(transferData);

    return result.release();
}

//
// Histo1DSink
//

static const size_t HistoMemAlignment = 64;

Histo1DSink::Histo1DSink(QObject *parent)
    : BasicSink(parent)
    , m_rrf(AxisBinning::NoResolutionReduction)
{
}

void Histo1DSink::beginRun(const RunInfo &runInfo, Logger logger)
{
    /* Single memory block allocation strategy:
     * Don't shrink.
     * If resizing to a larger size, recreate the arena. This will invalidate
     * all pointers into the histograms. Recreate pointers into arena, clear
     * memory. Update pointers for existing Histo1D instances.
     */

    // Check for invalid inpput
    if (!m_inputSlot.isParamIndexInRange())
    {
        m_histos.resize(0);
        m_histoArena.reset();
        return;
    }

    s32 histoCount = 0;
    s32 minIdx = 0;
    s32 maxIdx = 0;

    if (m_inputSlot.paramIndex != Slot::NoParamIndex)
    {
        histoCount = 1;
        minIdx = m_inputSlot.paramIndex;
        maxIdx = minIdx + 1;
    }
    else
    {
        histoCount = m_inputSlot.inputPipe->parameters.size();
        minIdx = 0;
        maxIdx = (s32)histoCount;
    }

    bool histoCountChanged   = m_histos.size() != histoCount;
    bool binCountChanged = true;
    if (!m_histos.isEmpty() && m_histos[0]->getNumberOfBins() == static_cast<u32>(m_bins))
    {
        binCountChanged = false;
    }
    bool structureChanged = histoCountChanged || binCountChanged;

    m_histos.resize(histoCount);

    // Space for the histos plus space to allow proper alignment
    size_t requiredMemory = (histoCount * m_bins * sizeof(double)
                             + histoCount * HistoMemAlignment);

    if (!m_histoArena || m_histoArena->size() < requiredMemory)
    {
        assert(structureChanged);
        // Have to (re)alloc as we either have no memory yet or not enough.
        m_histoArena = std::make_shared<memory::Arena>(requiredMemory);
    }
    else if (structureChanged)
    {
        // clear arena. histo data will be reassigned below
        m_histoArena->reset();
    }
    else
    {
        // structure did not change, leave arena intact.
    }

    for (s32 idx = minIdx, histoIndex = 0; idx < maxIdx; idx++, histoIndex++)
    {
        SharedHistoMem histoMem;
        auto histo = m_histos[histoIndex];

        if (!structureChanged)
        {
            assert(histo);
            histoMem = histo->getSharedMemory();
            assert(histoMem.size == m_bins);
        }
        else
        {
            histoMem =
            {
                m_histoArena,
                m_histoArena->pushArray<double>(m_bins, HistoMemAlignment),
                m_bins
            };
        }

        assert(histoMem.data);

        double xMin = m_xLimitMin;
        double xMax = m_xLimitMax;

        if (std::isnan(xMin))
        {
            xMin = m_inputSlot.inputPipe->parameters[idx].lowerLimit;
        }

        if (std::isnan(xMax))
        {
            xMax = m_inputSlot.inputPipe->parameters[idx].upperLimit;
        }

        AxisBinning binning(m_bins, xMin, xMax);

        if (histo)
        {
            assert(!histo->ownsMemory());
            histo->setData(histoMem, binning);
        }
        else
        {
            m_histos[histoIndex] = histo = std::make_shared<Histo1D>(binning, histoMem);
        }

        assert(histo);

        auto histoName = this->objectName();
        AxisInfo axisInfo;
        axisInfo.title = this->m_xAxisTitle;
        axisInfo.unit  = m_inputSlot.inputPipe->parameters.unit;

        if (maxIdx - minIdx > 1)
        {
            histoName = QString("%1[%2]").arg(histoName).arg(idx);
            axisInfo.title = QString("%1[%2]").arg(axisInfo.title).arg(idx);
        }
        histo->setObjectName(histoName);
        histo->setAxisInfo(Qt::XAxis, axisInfo);
        histo->setTitle(histoName);

        if (!runInfo.runId.isEmpty())
        {
            histo->setFooter(QString("<small>runId=%1</small>").arg(runInfo.runId));
        }
    }

    if (!runInfo.keepAnalysisState)
    {
        clearState();
    }
}

void Histo1DSink::clearState()
{
    qDebug() << __PRETTY_FUNCTION__ << objectName();
    for (auto &histo: m_histos)
    {
        histo->clear();
    }
}

void Histo1DSink::read(const QJsonObject &json)
{
    m_bins = json["nBins"].toInt();
    m_xAxisTitle = json["xAxisTitle"].toString();
    m_xLimitMin = json["xLimitMin"].toDouble(make_quiet_nan());
    m_xLimitMax = json["xLimitMax"].toDouble(make_quiet_nan());
    m_rrf = json["resolutionReductionFactor"].toInt(Histo1D::NoRR);

    Q_ASSERT(m_bins > 0);
}

void Histo1DSink::write(QJsonObject &json) const
{
    json["nBins"] = static_cast<qint64>(m_bins);
    json["xAxisTitle"] = m_xAxisTitle;
    json["xLimitMin"]  = m_xLimitMin;
    json["xLimitMax"]  = m_xLimitMax;
    json["resolutionReductionFactor"] = static_cast<qint64>(getResolutionReductionFactor());
}

size_t Histo1DSink::getStorageSize() const
{
    return m_histoArena ? m_histoArena->size() : 0;
}

//
// Histo2DSink
//
Histo2DSink::Histo2DSink(QObject *parent)
    : SinkInterface(parent)
    , m_inputX(this, 0, QSL("X-Axis"), InputType::Value)
    , m_inputY(this, 1, QSL("Y-Axis"), InputType::Value)
{
}

// Creates or resizes the histogram. Updates the axis limits to match
// the input parameters limits. Clears the histogram.
void Histo2DSink::beginRun(const RunInfo &runInfo, Logger logger)
{
    if (m_inputX.inputPipe && m_inputY.inputPipe)
    {
        auto sourceX = m_inputX.inputPipe->getSource();
        auto sourceY = m_inputY.inputPipe->getSource();

        qDebug() << __PRETTY_FUNCTION__
            << metaObject()->className()
            << objectName() << getId()
            << "sourceX =" << sourceX << ", flagsX =" << to_string(sourceX->getObjectFlags())
            << ", paramIndexX =" << m_inputX.paramIndex
            << "\n"
            << "sourceY =" << sourceY << ", flagsY =" << to_string(sourceY->getObjectFlags())
            << ", paramIndexY =" << m_inputY.paramIndex
            << "\n"
            << ", required_inputs_connected_and_valid =" << required_inputs_connected_and_valid(this)
            ;
    }

    if (m_inputX.inputPipe && m_inputY.inputPipe
        && 0 <= m_inputX.paramIndex && m_inputX.paramIndex < m_inputX.inputPipe->parameters.size()
        && 0 <= m_inputY.paramIndex && m_inputY.paramIndex < m_inputY.inputPipe->parameters.size())
    {
        auto sourceX = m_inputX.inputPipe->getSource();
        auto sourceY = m_inputY.inputPipe->getSource();

        double xMin = m_xLimitMin;
        double xMax = m_xLimitMax;

        if (std::isnan(xMin))
        {
            xMin = m_inputX.inputPipe->parameters[m_inputX.paramIndex].lowerLimit;
        }

        if (std::isnan(xMax))
        {
            xMax = m_inputX.inputPipe->parameters[m_inputX.paramIndex].upperLimit;
        }

        double yMin = m_yLimitMin;
        double yMax = m_yLimitMax;

        if (std::isnan(yMin))
        {
            yMin = m_inputY.inputPipe->parameters[m_inputY.paramIndex].lowerLimit;
        }

        if (std::isnan(yMax))
        {
            yMax = m_inputY.inputPipe->parameters[m_inputY.paramIndex].upperLimit;
        }

        if (!m_histo)
        {
            m_histo = std::make_shared<Histo2D>(m_xBins, xMin, xMax,
                                                m_yBins, yMin, yMax);

        }
        else
        {
            if (m_histo->getAxisBinning(Qt::XAxis).getBins() != static_cast<u32>(m_xBins)
                || m_histo->getAxisBinning(Qt::YAxis).getBins() != static_cast<u32>(m_yBins)
                || !runInfo.keepAnalysisState)
            {
                // resize always implicitly clears
                m_histo->resize(m_xBins, m_yBins);
            }

            AxisBinning newXBinning(m_xBins, xMin, xMax);
            AxisBinning newYBinning(m_yBins, yMin, yMax);

            if (m_histo->getAxisBinning(Qt::XAxis) != newXBinning
                || m_histo->getAxisBinning(Qt::YAxis) != newYBinning)
            {
                m_histo->setAxisBinning(Qt::XAxis, newXBinning);
                m_histo->setAxisBinning(Qt::YAxis, newYBinning);
                m_histo->clear(); // have to clear because the binning changed
            }
        }

        m_histo->setObjectName(objectName());
        m_histo->setTitle(objectName());

        if (!runInfo.runId.isEmpty())
        {
            m_histo->setFooter(QString("<small>runId=%1</small>").arg(runInfo.runId));
        }

        {
            AxisInfo info;
            info.title = m_xAxisTitle;
            info.unit  = m_inputX.inputPipe->parameters.unit;
            m_histo->setAxisInfo(Qt::XAxis, info);
        }

        {
            AxisInfo info;
            info.title = m_yAxisTitle;
            info.unit  = m_inputY.inputPipe->parameters.unit;
            m_histo->setAxisInfo(Qt::YAxis, info);
        }
    }
}

void Histo2DSink::clearState()
{
    qDebug() << __PRETTY_FUNCTION__ << objectName();
    if (m_histo)
    {
        m_histo->clear();
    }
}

s32 Histo2DSink::getNumberOfSlots() const
{
    return 2;
}

Slot *Histo2DSink::getSlot(s32 slotIndex)
{
    switch (slotIndex)
    {
        case 0:
            return &m_inputX;
        case 1:
            return &m_inputY;
        default:
            return nullptr;
    }
}

void Histo2DSink::read(const QJsonObject &json)
{
    m_xBins = static_cast<s32>(json["xBins"].toInt());
    m_xLimitMin = json["xLimitMin"].toDouble(make_quiet_nan());
    m_xLimitMax = json["xLimitMax"].toDouble(make_quiet_nan());

    m_yBins = static_cast<s32>(json["yBins"].toInt());
    m_yLimitMin = json["yLimitMin"].toDouble(make_quiet_nan());
    m_yLimitMax = json["yLimitMax"].toDouble(make_quiet_nan());

    m_xAxisTitle = json["xAxisTitle"].toString();
    m_yAxisTitle = json["yAxisTitle"].toString();

    m_rrf.x = json["rrfX"].toInt(AxisBinning::NoResolutionReduction);
    m_rrf.y = json["rrfY"].toInt(AxisBinning::NoResolutionReduction);
}

void Histo2DSink::write(QJsonObject &json) const
{
    json["xBins"] = m_xBins;
    json["xLimitMin"]  = m_xLimitMin;
    json["xLimitMax"]  = m_xLimitMax;

    json["yBins"] = m_yBins;
    json["yLimitMin"]  = m_yLimitMin;
    json["yLimitMax"]  = m_yLimitMax;

    json["xAxisTitle"] = m_xAxisTitle;
    json["yAxisTitle"] = m_yAxisTitle;

    json["rrfX"] = static_cast<qint64>(m_rrf.x);
    json["rrfY"] = static_cast<qint64>(m_rrf.y);
}

size_t Histo2DSink::getStorageSize() const
{
    return m_histo ? m_histo->getStorageSize() : 0u;
}

//
// RateMonitorSink
//

static QString to_string(RateMonitorSink::Type type)
{
    QString result;

    switch (type)
    {
        case RateMonitorSink::Type::PrecalculatedRate:
            result = QSL("PrecalculatedRate");
            break;
        case RateMonitorSink::Type::CounterDifference:
            result = QSL("CounterDifference");
            break;
        case RateMonitorSink::Type::FlowRate:
            result = QSL("FlowRate");
            break;
    }

    return result;
}

static RateMonitorSink::Type rate_monitor_sink_type_from_string(const QString &str)
{
    RateMonitorSink::Type result = RateMonitorSink::Type::CounterDifference;

    if (str.compare(QSL("PrecalculatedRate"), Qt::CaseInsensitive) == 0)
        result = RateMonitorSink::Type::PrecalculatedRate;

    if (str.compare(QSL("CounterDifference"), Qt::CaseInsensitive) == 0)
        result = RateMonitorSink::Type::CounterDifference;

    if (str.compare(QSL("FlowRate"), Qt::CaseInsensitive) == 0)
        result = RateMonitorSink::Type::FlowRate;

    return result;
}

RateMonitorSink::RateMonitorSink(QObject *parent)
    : SinkInterface(parent)
{
    addSlot();
}

bool RateMonitorSink::addSlot()
{
    auto inputType = InputType::Both;

    auto slot = std::make_shared<Slot>(
        this, getNumberOfSlots(),
        QSL("Input #") + QString::number(getNumberOfSlots()), inputType);

    m_inputs.push_back(slot);

    return true;
}

bool RateMonitorSink::removeLastSlot()
{
    if (m_inputs.size() > 1)
    {
        m_inputs.back()->disconnectPipe();
        m_inputs.pop_back();
        return true;
    }

    return false;
}

Slot *RateMonitorSink::getSlot(s32 slotIndex)
{
    return m_inputs.value(slotIndex).get();
}

s32 RateMonitorSink::getNumberOfSlots() const
{
    return m_inputs.size();
}

void RateMonitorSink::beginRun(const RunInfo &runInfo, Logger logger)
{
    m_samplerInputMapping.resize(0);

    if (no_input_connected(this))
    {
        m_samplers.resize(0);
        m_inputSamplerOffsets.resize(0);
        return;
    }

    m_inputSamplerOffsets.resize(getNumberOfSlots());

    size_t requiredSamplers = 0u;

    for (s32 ii = 0; ii < getNumberOfSlots(); ii++)
    {
        auto slot = getSlot(ii);

        if (!slot->isConnected())
        {
            m_inputSamplerOffsets[ii] = -1;
        }
        else
        {
            m_inputSamplerOffsets[ii] = requiredSamplers;

            if (slot->isParameterConnection())
            {
                requiredSamplers += 1u;
                m_samplerInputMapping.push_back(ii);
            }
            else
            {
                requiredSamplers += slot->inputPipe->getSize();

                for (s32 pi = 0; pi < slot->inputPipe->getSize(); pi++)
                {
                    m_samplerInputMapping.push_back(ii);
                }
            }
        }
    }

    assert(static_cast<size_t>(m_samplerInputMapping.size()) == requiredSamplers);
    m_samplers.resize(requiredSamplers);

    for (auto &sampler: m_samplers)
    {
        if (!sampler)
        {
            sampler = std::make_shared<a2::RateSampler>();
            sampler->rateHistory = RateHistoryBuffer(m_rateHistoryCapacity);
        }
        else
        {
            sampler->lastValue = 0.0;
            sampler->lastRate  = 0.0;
            sampler->lastDelta = 0.0;

            /* If the new capacity is >= the old capacity then the rateHistory
             * contents are kept, otherwise the oldest values are discarded. */
            if (sampler->rateHistory.capacity() != m_rateHistoryCapacity)
            {
                sampler->rateHistory.set_capacity(m_rateHistoryCapacity);
                sampler->rateHistory.resize(0);
                sampler->totalSamples = 0.0;
            }

            if (!runInfo.keepAnalysisState)
            {
                // truncates the history size (not the capacity) to zero
                sampler->clearHistory();
            }
        }

        sampler->scale = getCalibrationFactor();
        sampler->offset = getCalibrationOffset();
        sampler->interval = getSamplingInterval();

        assert(sampler->rateHistory.capacity() == m_rateHistoryCapacity);
        assert(runInfo.keepAnalysisState || sampler->rateHistory.size() == 0);
        assert(sampler->scale == getCalibrationFactor());
        assert(sampler->offset == getCalibrationOffset());
        assert(sampler->interval == getSamplingInterval());
    }
}

void RateMonitorSink::clearState()
{
    qDebug() << __PRETTY_FUNCTION__ << objectName();

    for (auto &sampler: m_samplers)
    {
        sampler->clearHistory();
    }
}

void RateMonitorSink::write(QJsonObject &json) const
{
    json["type"] = to_string(getType());
    json["capacity"] = static_cast<qint64>(m_rateHistoryCapacity);
    json["unitLabel"] = getUnitLabel();
    json["calibrationFactor"] = getCalibrationFactor();
    json["calibrationOffset"] = getCalibrationOffset();
    json["samplingInterval"]  = getSamplingInterval();
    json["numberOfInputs"] = getNumberOfSlots();
    json["useCombinedView"] = getUseCombinedView();
}

void RateMonitorSink::read(const QJsonObject &json)
{
    for (auto &slot: m_inputs)
    {
        slot->disconnectPipe();
    }
    m_inputs.clear();
    m_samplers.clear();

    // Default to 1 to enable reading of older analysis files that contain the single
    // input version of the RateMonitorSink
    s32 inputCount = json["numberOfInputs"].toInt(1);

    for (s32 inputIndex = 0;
         inputIndex < inputCount;
         ++inputIndex)
    {
        addSlot();
    }

    m_type = rate_monitor_sink_type_from_string(json["type"].toString());
    m_rateHistoryCapacity = json["capacity"].toInt();
    m_unitLabel = json["unitLabel"].toString();
    m_calibrationFactor = json["calibrationFactor"].toDouble(1.0);
    m_calibrationOffset = json["m_calibrationOffset"].toDouble(0.0);
    m_samplingInterval  = json["samplingInterval"].toDouble(1.0);
    m_useCombinedView   = json["useCombinedView"].toBool(false);
}

size_t RateMonitorSink::getStorageSize() const
{
    return std::accumulate(m_samplers.begin(), m_samplers.end(),
                           static_cast<size_t>(0u),
                           [](size_t accu, const a2::RateSamplerPtr &sampler) {
        return accu + sampler->rateHistory.capacity() * sizeof(double);
   });
}

//
// ExportSink
//
ExportSink::ExportSink(QObject *parent)
    : SinkInterface(parent)
    , m_conditionInput(this, 0, "Condition Input (optional)", InputType::Value)
{
    m_conditionInput.isOptional = true;
    addSlot();
}

bool ExportSink::addSlot()
{
    auto inputType = InputType::Array;

    auto slot = std::make_shared<Slot>(
        this, getNumberOfSlots(),
        QSL("Data Input #") + QString::number(getNumberOfSlots()), inputType);

    m_dataInputs.push_back(slot);

    return true;
}

bool ExportSink::removeLastSlot()
{
    if (m_dataInputs.size() > 1)
    {
        m_dataInputs.back()->disconnectPipe();
        m_dataInputs.pop_back();
        return true;
    }

    return false;
}

Slot *ExportSink::getSlot(s32 slotIndex)
{
    Slot *result = nullptr;

    if (slotIndex == 0)
    {
        result = &m_conditionInput;
    }
    else if (slotIndex - 1 < m_dataInputs.size())
    {
        result = m_dataInputs.at(slotIndex - 1).get();
    }

    return result;
}

s32 ExportSink::getNumberOfSlots() const
{
    return 1 + m_dataInputs.size();
}

void ExportSink::beginRun(const RunInfo &, Logger logger)
{
    if (getOutputPrefixPath().isEmpty())
        return;

    if (!QDir().mkpath(getOutputPrefixPath()))
    {
        if (logger)
        {
            logger(QString("ExportSink %1: Error creating export directory %2")
                   .arg(this->objectName())
                   .arg(getOutputPrefixPath()));
        }
    }
}

void ExportSink::generateCode(Logger logger)
{
    try
    {
        if (!QDir().mkpath(getOutputPrefixPath()))
        {
            auto msg = QSL("Could not create export directory %1")
                .arg(getOutputPrefixPath());

            throw std::runtime_error(msg.toStdString());
        }

        ExportSinkCodeGenerator codeGen(this);
        codeGen.generateFiles(logger);

        qDebug() << __PRETTY_FUNCTION__ << codeGen.getOutputFilenames();
    }
    catch (const std::exception &e)
    {
        if (logger)
        {
            auto msg = QSL("Error during code generation: %1")
                .arg(e.what());
            logger(msg);
        }
    }
}

QStringList ExportSink::getOutputFilenames()
{
    return ExportSinkCodeGenerator(this).getOutputFilenames();
}

void ExportSink::write(QJsonObject &json) const
{
    json["dataInputCount"]   = m_dataInputs.size();
    json["outputPrefixPath"] = getOutputPrefixPath();
    json["compressionLevel"] = getCompressionLevel();
    json["format"]           = static_cast<s32>(getFormat());
}

void ExportSink::read(const QJsonObject &json)
{
    m_dataInputs.clear();

    s32 inputCount = json["dataInputCount"].toInt();

    for (s32 inputIndex = 0;
         inputIndex < inputCount;
         ++inputIndex)
    {
        addSlot();
    }

    setOutputPrefixPath(json["outputPrefixPath"].toString());
    setCompressionLevel(json["compressionLevel"].toInt());
    setFormat(static_cast<Format>(json["format"].toInt(static_cast<s32>(Format::Sparse))));
}

void ExportSink::postClone(const AnalysisObject *cloneSource)
{
    auto cs = qobject_cast<const ExportSink *>(cloneSource);
    assert(cs);

    m_outputPrefixPath = cs->getOutputPrefixPath() + "_copy";
    SinkInterface::postClone(cloneSource);
}

QString ExportSink::getDataFilePath(const RunInfo &runInfo) const
{
    QString result = getOutputPrefixPath() + "/" + getDataFileName(runInfo);

    return result;
}

QString ExportSink::getDataFileExtension() const
{
    QString result = ".bin";

    if (m_compressionLevel != 0)
    {
        result += ".gz";
    }

    return result;
}

QString ExportSink::getDataFileName(const RunInfo &runInfo) const
{
    return (QString("data_%1%2")
            .arg(runInfo.runId)
            .arg(getDataFileExtension()));
}

QString ExportSink::getExportFileBasename() const
{
    return QFileInfo(getOutputPrefixPath()).baseName();
}

//
// Condition Support
//

template<typename It>
void normalize_intervals(It first, It onePastEnd)
{
    while (first != onePastEnd)
    {
        *first = first->normalized();
        first++;
    }
}

QJsonObject to_json(const QwtInterval &interval)
{
    QJsonObject result;
    result["min"] = interval.minValue();
    result["max"] = interval.maxValue();
    return result;
}

QwtInterval interval_from_json(const QJsonObject &json)
{
    QwtInterval result;
    result.setMinValue(json["min"].toDouble(make_quiet_nan()));
    result.setMaxValue(json["max"].toDouble(make_quiet_nan()));
    return result;
}

QJsonObject to_json(const QPointF &point)
{
    QJsonObject result;
    result["x"] = point.x();
    result["y"] = point.y();
    return result;
}

QPointF qpointf_from_json(const QJsonObject &json)
{
    return { json["x"].toDouble(), json["y"].toDouble() };
}

QJsonObject to_json(const QRectF &rect)
{
    QJsonObject result;

    result["topLeft"] = to_json(rect.topLeft());
    result["bottomRight"] = to_json(rect.bottomRight());

    return result;
}

QRectF qrectf_from_json(const QJsonObject &json)
{
    QRectF result(qpointf_from_json(json["topLeft"].toObject()),
                  qpointf_from_json(json["bottomRight"].toObject()));

    return result;
}

QJsonArray to_json(const QPolygonF &poly)
{
    QJsonArray points;

    for (const auto &point: poly)
    {
        points.append(to_json(point));
    }

    return points;
}

QPolygonF qpolygonf_from_json(const QJsonArray &points)
{
    QPolygonF result;
    result.reserve(points.size());

    for (auto it = points.begin(); it != points.end(); it++)
    {
        result.append(qpointf_from_json(it->toObject()));
    }

    return result;
}

//
// ConditionInterval
//

ConditionInterval::ConditionInterval(QObject *parent)
    : ConditionInterface(parent)
    , m_input(this, 0, QSL("Input"), InputType::Array)
{
}

void ConditionInterval::beginRun(const RunInfo &runInfo, Logger logger)
{
    if (m_input.isConnected())
    {
        // Discards any additional intervals and adds invalid intervals to the
        // end if needed.
        m_intervals.resize(m_input.inputPipe->getSize());
    }
}

void ConditionInterval::write(QJsonObject &json) const
{
    QJsonArray jsonIntervals;

    for (const auto &interval: m_intervals)
    {
        jsonIntervals.append(to_json(interval));
    }

    json["intervals"] = jsonIntervals;
}

void ConditionInterval::read(const QJsonObject &json)
{
    m_intervals.clear();

    auto jsonIntervals = json["intervals"].toArray();

    for (auto it = jsonIntervals.begin(); it != jsonIntervals.end(); it++)
    {
        m_intervals.append(interval_from_json(it->toObject()));
    }
}

s32 ConditionInterval::getNumberOfSlots() const
{
    return 1;
}

Slot *ConditionInterval::getSlot(s32 slotIndex)
{
    return slotIndex == 0 ? &m_input : nullptr;
}

void ConditionInterval::setIntervals(const QVector<QwtInterval> &intervals)
{
    m_intervals = intervals;
    normalize_intervals(m_intervals.begin(), m_intervals.end());
}

QVector<QwtInterval> ConditionInterval::getIntervals() const
{
    return m_intervals;
}

void ConditionInterval::setInterval(s32 address, const QwtInterval &interval)
{
    if (address >= 0)
    {
        m_intervals.resize(std::max(m_intervals.size(), address+1));

        assert(address < m_intervals.size());

        m_intervals[address] = interval.normalized();
    }
}

QwtInterval ConditionInterval::getInterval(s32 address) const
{
    return m_intervals.value(address, { make_quiet_nan(), make_quiet_nan() });
}

s32 ConditionInterval::getNumberOfBits() const
{
    return m_input.isConnected() ? m_input.inputPipe->getSize() : 0u;
}

//
// ConditionRectangle
//
ConditionRectangle::ConditionRectangle(QObject *parent)
    : ConditionInterface(parent)
    , m_inputX(this, 0, QSL("X Input"), InputType::Value)
    , m_inputY(this, 1, QSL("Y Input"), InputType::Value)
{
}

void ConditionRectangle::write(QJsonObject &json) const
{
    json["rectangle"] = to_json(m_rectangle);
}

void ConditionRectangle::read(const QJsonObject &json)
{
    m_rectangle = qrectf_from_json(json["rectangle"].toObject());
}

s32 ConditionRectangle::getNumberOfSlots() const
{
    return 2;
}

Slot *ConditionRectangle::getSlot(s32 slotIndex)
{
    switch (slotIndex)
    {
        case 0: return &m_inputX;
        case 1: return &m_inputY;
        default: break;
    }

    return nullptr;
}

void ConditionRectangle::beginRun(const RunInfo &runInfo, Logger logger)
{
}

void ConditionRectangle::setRectangle(const QRectF &rect)
{
    m_rectangle = rect;
}

QRectF ConditionRectangle::getRectangle() const
{
    return m_rectangle;
}

//
// ConditionPolygon
//
ConditionPolygon::ConditionPolygon(QObject *parent)
    : ConditionInterface(parent)
    , m_inputX(this, 0, QSL("X Input"), InputType::Value)
    , m_inputY(this, 1, QSL("Y Input"), InputType::Value)
{
}

void ConditionPolygon::write(QJsonObject &json) const
{
    json["polygon"] = to_json(m_polygon);
}

void ConditionPolygon::read(const QJsonObject &json)
{
    m_polygon = qpolygonf_from_json(json["polygon"].toArray());
}

s32 ConditionPolygon::getNumberOfSlots() const
{
    return 2;
}

Slot *ConditionPolygon::getSlot(s32 slotIndex)
{
    switch (slotIndex)
    {
        case 0: return &m_inputX;
        case 1: return &m_inputY;
        default: break;
    }

    return nullptr;
}

void ConditionPolygon::beginRun(const RunInfo &runInfo, Logger logger)
{
}

void ConditionPolygon::setPolygon(const QPolygonF &polygon)
{
    m_polygon = polygon;
}

QPolygonF ConditionPolygon::getPolygon() const
{
    return m_polygon;
}

//
// Analysis
//

static const size_t A2ArenaSegmentSize = Kilobytes(256);

Analysis::Analysis(QObject *parent)
    : QObject(parent)
    , m_modified(false)
    , m_timetickCount(0.0)
    , m_a2ArenaIndex(0)
{
    m_objectFactory.registerSource<ListFilterExtractor>();
    m_objectFactory.registerSource<Extractor>();

    m_objectFactory.registerOperator<CalibrationMinMax>();
    m_objectFactory.registerOperator<PreviousValue>();
    m_objectFactory.registerOperator<Difference>();
    m_objectFactory.registerOperator<Sum>();
    m_objectFactory.registerOperator<ArrayMap>();
    m_objectFactory.registerOperator<RangeFilter1D>();
    m_objectFactory.registerOperator<ConditionFilter>();
    m_objectFactory.registerOperator<RectFilter2D>();
    m_objectFactory.registerOperator<BinarySumDiff>();
    m_objectFactory.registerOperator<AggregateOps>();
    m_objectFactory.registerOperator<ExpressionOperator>();
    m_objectFactory.registerOperator<ConditionInterval>();
    m_objectFactory.registerOperator<ConditionRectangle>();
    m_objectFactory.registerOperator<ConditionPolygon>();

    m_objectFactory.registerSink<Histo1DSink>();
    m_objectFactory.registerSink<Histo2DSink>();
    m_objectFactory.registerSink<RateMonitorSink>();
    m_objectFactory.registerSink<ExportSink>();

    qDebug() << "Registered Sources:   " << m_objectFactory.getSourceNames();
    qDebug() << "Registered Operators: " << m_objectFactory.getOperatorNames();
    qDebug() << "Registered Sinks:     " << m_objectFactory.getSinkNames();

    // create a2 arenas
    for (size_t i = 0; i < m_a2Arenas.size(); i++)
    {
        m_a2Arenas[i] = std::make_unique<memory::Arena>(A2ArenaSegmentSize);
    }
    m_a2WorkArena = std::make_unique<memory::Arena>(A2ArenaSegmentSize);
}

Analysis::~Analysis()
{
}

//
// Data Sources
//

SourceVector Analysis::getSources(const QUuid &eventId, const QUuid &moduleId) const
{
    SourceVector result;

    for (const auto &s: m_sources)
    {
        if (s->getEventId() == eventId && s->getModuleId() == moduleId)
            result.push_back(s);
    }

    return result;
}

SourceVector Analysis::getSourcesByModule(const QUuid &moduleId) const
{
    SourceVector result;

    for (const auto &s: m_sources)
    {
        if (s->getModuleId() == moduleId)
            result.push_back(s);
    }

    return result;
}

SourceVector Analysis::getSourcesByEvent(const QUuid &eventId) const
{
    SourceVector result;

    for (const auto &s: m_sources)
    {
        if (s->getEventId() == eventId)
            result.push_back(s);
    }

    return result;
}

SourcePtr Analysis::getSource(const QUuid &sourceId) const
{
    auto it = std::find_if(m_sources.begin(), m_sources.end(),
                           [sourceId](const SourcePtr &src) {
        return src->getId() == sourceId;
    });

    return it != m_sources.end() ? *it : nullptr;
}

void Analysis::addSource(const QUuid &eventId, const QUuid &moduleId, const SourcePtr &source)
{
    source->setEventId(eventId);
    source->setModuleId(moduleId);
    addSource(source);
}

void Analysis::addSource(const SourcePtr &source)
{
    m_sources.push_back(source);
    source->setObjectFlags(ObjectFlags::NeedsRebuild);
    setModified();
    emit dataSourceAdded(source);
}

void Analysis::removeSource(const SourcePtr &source)
{
    assert(source);

    auto it = std::find_if(m_sources.begin(), m_sources.end(),
                           [source](const SourcePtr &src) {
        return src == source;
    });

    if (it != m_sources.end())
    {
        // Mark all dependees as needing a rebuild
        for (auto &obj: collect_dependent_objects(source))
        {
            obj->setObjectFlags(ObjectFlags::NeedsRebuild);
        }

        // Remove our output pipes from any connected slots.
        for (s32 outputIndex = 0;
             outputIndex < source->getNumberOfOutputs();
             outputIndex++)
        {
            Pipe *outPipe = source->getOutput(outputIndex);
            for (Slot *destSlot: outPipe->getDestinations())
            {
                destSlot->disconnectPipe();
            }
            assert(outPipe->getDestinations().isEmpty());
        }

        m_sources.erase(it);
        setModified();
        emit dataSourceRemoved(source);
    }
}

void Analysis::removeSource(SourceInterface *source)
{
    removeSource(std::dynamic_pointer_cast<SourceInterface>(source->shared_from_this()));
}

void Analysis::setSourceEdited(const SourcePtr &source)
{
    setModified();

    source->setObjectFlags(ObjectFlags::NeedsRebuild);

    for (auto &obj: collect_dependent_objects(source.get()))
        obj->setObjectFlags(ObjectFlags::NeedsRebuild);

    emit dataSourceEdited(source);
}

ListFilterExtractorVector Analysis::getListFilterExtractors(const QUuid &eventId,
                                                            const QUuid &moduleId) const
{
    ListFilterExtractorVector result;

    for (const auto &source: getSources(eventId, moduleId))
    {
        if (auto lfe = std::dynamic_pointer_cast<ListFilterExtractor>(source))
        {
            result.push_back(lfe);
        }
    }

    return result;
}

template<typename C>
QSet<typename C::value_type> set_from_container(const C &container)
{
    QSet<typename C::value_type> result;

    for (const auto &obj: container)
    {
        result.insert(obj);
    }

    return result;
}

// Replace the list of ListFilterExtractor attached to the given eventId and
// moduleId with the given list of extractors.
void Analysis::setListFilterExtractors(const QUuid &eventId,
                                       const QUuid &moduleId,
                                       const ListFilterExtractorVector &extractors)
{
    auto currentFilters = set_from_container(getListFilterExtractors(eventId, moduleId));
    auto givenFilters   = set_from_container(extractors);
    auto newFilters     = givenFilters - currentFilters;
    auto remFilters     = currentFilters - givenFilters;

    qDebug() << __PRETTY_FUNCTION__
        << "counts: current=" << currentFilters.size()
        << ", given=" << givenFilters.size()
        << ", new=" << newFilters.size()
        << ", rem=" << remFilters.size()
        << endl
        << "current=" << currentFilters << endl << endl
        << "given=" << givenFilters << endl << endl
        << "new=" << newFilters << endl << endl
        << "rem=" << remFilters << endl << endl
        ;

    auto listfilter_match = [&eventId, &moduleId] (const SourcePtr &source) -> bool
    {
        return (qobject_cast<ListFilterExtractor *>(source.get())
                && source->getEventId() == eventId
                && source->getModuleId() == moduleId);
    };

    // remove all existing listfilters from our sources
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(), listfilter_match),
                    m_sources.end());

    // add the listfilters passed in
    for (auto lfe: extractors)
    {
        lfe->setEventId(eventId);
        lfe->setModuleId(moduleId);
        lfe->setObjectFlags(ObjectFlags::NeedsRebuild);
        m_sources.push_back(lfe);
    }

    // handle filters that got removed
    for (auto &lfe: remFilters)
    {
        for (auto &obj: collect_dependent_objects(lfe.get()))
        {
            obj->setObjectFlags(ObjectFlags::NeedsRebuild);
        }

        disconnect_outputs(lfe.get());
        emit dataSourceRemoved(lfe);
    }

    // handle new filters
    for (auto &lfe: newFilters)
    {
        emit dataSourceAdded(lfe);
    }
}

//
// Operators
//

OperatorVector Analysis::getOperators(const QUuid &eventId) const
{
    OperatorVector result;

    for (const auto &op: m_operators)
    {
        if (op->getEventId() == eventId)
            result.push_back(op);
    }

    return result;
}

OperatorVector Analysis::getOperators(const QUuid &eventId, s32 userLevel) const
{
    OperatorVector result;

    for (const auto &op: m_operators)
    {
        if (op->getEventId() == eventId && op->getUserLevel() == userLevel)
            result.push_back(op);
    }

    return result;
}

OperatorPtr Analysis::getOperator(const QUuid &operatorId) const
{
    auto it = std::find_if(m_operators.begin(), m_operators.end(),
                           [operatorId](const OperatorPtr &op) {
        return op->getId() == operatorId;
    });

    return it != m_operators.end() ? *it : nullptr;
}

OperatorVector Analysis::getNonSinkOperators() const
{
    OperatorVector result;

    for (const auto &op: m_operators)
    {
        if (!qobject_cast<SinkInterface *>(op.get()))
        {
            result.push_back(op);
        }
    }

    return result;
}

OperatorVector Analysis::getSinkOperators() const
{
    OperatorVector result;

    for (const auto &op: m_operators)
    {
        if (qobject_cast<SinkInterface *>(op.get()))
        {
            result.push_back(op);
        }
    }

    return result;
}

void Analysis::addOperator(const QUuid &eventId, s32 userLevel, const OperatorPtr &op)
{
    op->setEventId(eventId);
    op->setUserLevel(userLevel);
    addOperator(op);
}

void Analysis::addOperator(const OperatorPtr &op)
{
    op->setObjectFlags(ObjectFlags::NeedsRebuild);
    m_operators.push_back(op);
    setModified();
    emit operatorAdded(op);
}

void Analysis::removeOperator(const OperatorPtr &op)
{
    assert(op);

    auto it = std::find_if(m_operators.begin(), m_operators.end(),
                           [op](const OperatorPtr &op_) {
        return op_ == op;
    });

    if (it != m_operators.end())
    {
        for (auto &obj: collect_dependent_objects(op))
        {
            obj->setObjectFlags(ObjectFlags::NeedsRebuild);
        }

        // Remove pipe connections to our input slots.
        for (s32 si = 0; si < op->getNumberOfSlots(); si++)
        {
            Slot *inputSlot = op->getSlot(si);
            assert(inputSlot);
            inputSlot->disconnectPipe();
            assert(!inputSlot->inputPipe);
        }

        // Remove our output pipes from any connected slots.
        for (s32 oi = 0; oi < op->getNumberOfOutputs(); oi++)
        {
            Pipe *outPipe = op->getOutput(oi);
            for (Slot *destSlot: outPipe->getDestinations())
            {
                destSlot->disconnectPipe();
            }
            assert(outPipe->getDestinations().isEmpty());
        }

        // If a condition is being removed clear all links pointing to it.
        if (auto cond = std::dynamic_pointer_cast<ConditionInterface>(op))
        {

            /* Note: cannot call clearConditionLink while iterating the
             * condition link hash because that method will modify the hash. */
            struct OpAndCl
            {
                OperatorPtr op;
                ConditionLink cl;
            };

            QVector<OpAndCl> linksToClear;

            for (auto it = m_conditionLinks.begin();
                 it != m_conditionLinks.end();
                 it++)
            {
                const auto &cl = it.value();
                const auto &op = it.key();

                if (cl.condition == cond)
                {
                    linksToClear.push_back({ op, cl });
                }
            }

            for (auto &entry: linksToClear)
            {
                clearConditionLink(entry.op, entry.cl);
            }
        }

        m_operators.erase(it);
        setModified();
        emit operatorRemoved(op);
    }
}

void Analysis::removeOperator(OperatorInterface *op)
{
    removeOperator(std::dynamic_pointer_cast<OperatorInterface>(op->shared_from_this()));
}

void Analysis::setOperatorEdited(const OperatorPtr &op)
{
    setModified();

    op->setObjectFlags(ObjectFlags::NeedsRebuild);

    for (auto &obj: collect_dependent_objects(op.get()))
        obj->setObjectFlags(ObjectFlags::NeedsRebuild);

    emit operatorEdited(op);
}

ConditionVector Analysis::getConditions() const
{
    ConditionVector result;

    for (const auto &op: m_operators)
    {
        if (auto cond = std::dynamic_pointer_cast<ConditionInterface>(op))
        {
            result.push_back(cond);
        }
    }

    return result;
}

ConditionVector Analysis::getConditions(const QUuid &eventId) const
{

    auto pred = [eventId] (const ConditionPtr &cond)
    {
        return cond->getEventId() != eventId;
    };

    auto result = getConditions();

    result.erase(std::remove_if(result.begin(), result.end(), pred),
                 result.end());

    return result;
}

ConditionPtr Analysis::getCondition(const OperatorPtr &op) const
{
    return m_conditionLinks.value(op).condition;
}

ConditionLink Analysis::getConditionLink(const OperatorPtr &op) const
{
    return m_conditionLinks.value(op);
}

ConditionLink Analysis::getConditionLink(const OperatorInterface *op) const
{
    auto analysisObject = const_cast<OperatorInterface *>(op)->shared_from_this();

    return getConditionLink(std::dynamic_pointer_cast<OperatorInterface>(analysisObject));
}

ConditionLinks Analysis::getConditionLinks() const
{
    return m_conditionLinks;
}

bool Analysis::hasActiveCondition(const OperatorPtr &op) const
{
    return m_conditionLinks.value(op).condition != nullptr;
}

bool Analysis::setConditionLink(const OperatorPtr &op, const ConditionLink &cl)
{
    if (cl != m_conditionLinks.value(op))
    {
        m_conditionLinks.insert(op, cl);

        // Set rebuild flag to  clear operator state (e.g. histogram contents)
        // after the condition was set or changed.
        op->setObjectFlags(ObjectFlags::NeedsRebuild);

        emit conditionLinkApplied(op, cl);

        return true;
    }

    return true;
}

bool Analysis::clearConditionLink(const OperatorPtr &op, const ConditionLink &cl)
{
    if (m_conditionLinks.value(op) == cl)
    {
        m_conditionLinks.remove(op);
        op->setObjectFlags(ObjectFlags::NeedsRebuild);
        emit conditionLinkCleared(op, cl);
        return true;
    }

    return false;
}

bool Analysis::clearConditionLink(const OperatorPtr &op)
{
    if (auto cl = m_conditionLinks.value(op))
    {
        m_conditionLinks.remove(op);
        op->setObjectFlags(ObjectFlags::NeedsRebuild);
        emit conditionLinkCleared(op, cl);
        return true;
    }

    return false;
}

size_t Analysis::clearConditionLinksUsing(const ConditionInterface *cond)
{
    // Iterater over a copy as the call to clearConditionLink() modifies the
    // m_conditionLinks hash. */
    ConditionLinks links = m_conditionLinks;
    size_t result = 0u;

    for (auto it = links.begin(); it != links.end(); it++)
    {
        const auto &cl = it.value();
        const auto &op = it.key();

        if (cl.condition.get() == cond)
        {
            clearConditionLink(op, cl);
            result++;
        }
    }

    return result;
}

//
// Directories
//
const DirectoryVector Analysis::getDirectories(const QUuid &eventId,
                                               const DisplayLocation &loc) const
{
    DirectoryVector result;

    for (const auto &dir: m_directories)
    {
        if (dir->getEventId() == eventId
            && (loc == DisplayLocation::Any || dir->getDisplayLocation() == loc))
        {
            result.push_back(dir);
        }
    }

    return result;
}

const DirectoryVector Analysis::getDirectories(const QUuid &eventId, s32 userLevel,
                                               const DisplayLocation &loc) const
{
    DirectoryVector result;

    for (const auto &dir: m_directories)
    {
        if (dir->getEventId() == eventId
            && dir->getUserLevel() == userLevel
            && (loc == DisplayLocation::Any || dir->getDisplayLocation() == loc))
        {
            result.push_back(dir);
        }
    }

    return result;
}

DirectoryPtr Analysis::getDirectory(const QUuid &id) const
{
    for (const auto &dir: m_directories)
    {
        if (dir->getId() == id)
            return dir;
    }

    return nullptr;
}

void Analysis::setDirectories(const DirectoryVector &dirs)
{
    m_directories = dirs;
    setModified();
}

void Analysis::addDirectory(const DirectoryPtr &dir)
{
    qDebug() << __PRETTY_FUNCTION__;
    m_directories.push_back(dir);
    setModified();
    emit directoryAdded(dir);
}

void Analysis::removeDirectory(const DirectoryPtr &dir)
{
    int index = m_directories.indexOf(dir);
    removeDirectory(index);
}

void Analysis::removeDirectory(int index)
{
    assert(0 <= index && index < m_directories.size());
    auto dir = m_directories.value(index);
    m_directories.removeAt(index);
    setModified();
    emit directoryRemoved(dir);
}

DirectoryPtr Analysis::getParentDirectory(const AnalysisObjectPtr &obj) const
{
#ifndef QT_NO_DEBUG
    // Consistency check making sure that the object is not a member of multiple
    // directories.
    bool found = false;
    DirectoryPtr firstFoundDir;

    for (const auto &dir: m_directories)
    {
        if (dir->contains(obj))
        {
            if (found)
            {
                qDebug() << __PRETTY_FUNCTION__ << "object" << obj
                    << " is contained in multiple directories:"
                    << ", firstFoundDir =" << firstFoundDir.get()
                    << ", current dir =" << dir.get()
                    ;
            }

            assert(!found);
            found = true;
            firstFoundDir = dir;
        }
    }
#endif

    // Returns the first parent directory that contains the given object.
    for (const auto &dir: m_directories)
    {
        if (dir->contains(obj))
            return dir;
    }

    return nullptr;
}

AnalysisObjectVector Analysis::getDirectoryContents(const QUuid &directoryId) const
{
    return getDirectoryContents(getDirectory(directoryId));
}

AnalysisObjectVector Analysis::getDirectoryContents(const DirectoryPtr &dir) const
{
    return getDirectoryContents(dir.get());
}

AnalysisObjectVector Analysis::getDirectoryContents(const Directory *dir) const
{
    AnalysisObjectVector result;

    if (dir)
    {
        for (auto id: dir->getMembers())
        {
            result.push_back(getObject(id));
        }
    }

    return result;
}

AnalysisObjectVector Analysis::getDirectoryContentsRecursively(const QUuid &directoryId) const
{
    return getDirectoryContentsRecursively(getDirectory(directoryId));
}

AnalysisObjectVector Analysis::getDirectoryContentsRecursively(const DirectoryPtr &dir) const
{
    return getDirectoryContentsRecursively(dir.get());
}

AnalysisObjectVector Analysis::getDirectoryContentsRecursively(const Directory *dir) const
{
    AnalysisObjectVector result;

    auto objects = getDirectoryContents(dir);

    result += objects;

    for (auto &obj: objects)
    {
        if (auto subdir = std::dynamic_pointer_cast<Directory>(obj))
        {
            result += getDirectoryContentsRecursively(subdir);
        }
    }

    return result;
}

int Analysis::removeDirectoryRecursively(const DirectoryPtr &dir)
{
    auto objects = getDirectoryContents(dir);
    int removed = removeObjectsRecursively(objects);
    removeDirectory(dir);
    removed++;
    return removed;
}

namespace
{

class IdComparator
{
    public:
        IdComparator(const QUuid &idToMatch)
            : m_id(idToMatch)
        { }

        template<typename T>
        bool operator()(const T &obj)
        {
            return m_id == obj->getId();
        }

    private:
        QUuid m_id;
};

} // end anon namespace

AnalysisObjectPtr Analysis::getObject(const QUuid &id) const
{
    IdComparator cmp(id);

    {
        auto it = std::find_if(m_sources.begin(), m_sources.end(), cmp);

        if (it != m_sources.end())
            return *it;
    }

    {
        auto it = std::find_if(m_operators.begin(), m_operators.end(), cmp);

        if (it != m_operators.end())
            return *it;
    }

    {
        auto it = std::find_if(m_directories.begin(), m_directories.end(), cmp);

        if (it != m_directories.end())
            return *it;
    }

    return nullptr;
}

/* Removes the objects contained in the objects vector from the analysis.
 * If a directory is encountered its contents will be removed recursively. */
int Analysis::removeObjectsRecursively(const AnalysisObjectVector &objects)
{
    int removed = 0u;

    for (const auto &obj: objects)
    {
        if (auto source = std::dynamic_pointer_cast<SourceInterface>(obj))
        {
            removeSource(source);
            removed++;
        }
        else if (auto op = std::dynamic_pointer_cast<OperatorInterface>(obj))
        {
            removeOperator(op);
            removed++;
        }
        else if (auto dir = std::dynamic_pointer_cast<Directory>(obj))
        {
            removed += removeDirectoryRecursively(dir);
        }
    }

    return removed;
}

/* Returns a vector containing all child objects of this analysis. Object ordering is
 * preserved, this means, e.g. ListFilterExtractors will retain the correct ordering. */
AnalysisObjectVector Analysis::getAllObjects() const
{
    AnalysisObjectVector result;

    result.reserve(objectCount());

    for (const auto &obj: m_sources)
        result.append(obj);

    for (const auto &obj: m_operators)
        result.append(obj);

    for (const auto &obj: m_directories)
        result.append(obj);

    return result;
}

int Analysis::objectCount() const
{
    int result = 0;

    result += m_sources.size();
    result += m_operators.size();
    result += m_directories.size();

    return result;
}

void Analysis::addObjects(const AnalysisObjectStore &store)
{
    for (auto &obj: store.sources)
    {
        addSource(obj);
    }

    for (auto &obj: store.operators)
    {
        addOperator(obj);
    }

    for (auto &obj: store.directories)
    {
        addDirectory(obj);
    }
}

void Analysis::addObjects(const AnalysisObjectVector &objects)
{
    for (const auto &obj: objects)
    {
        if (auto src = std::dynamic_pointer_cast<SourceInterface>(obj))
        {
            addSource(src);
        }
        else if (auto op = std::dynamic_pointer_cast<OperatorInterface>(obj))
        {
            addOperator(op);
        }
        else if (auto dir = std::dynamic_pointer_cast<Directory>(obj))
        {
            addDirectory(dir);
        }
    }
}

//
// Pre and post run work
//

void Analysis::updateRanks()
{
#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << ">>>>> begin";
#endif

    for (auto src: m_sources)
    {
#if ENABLE_ANALYSIS_DEBUG
        qDebug() << __PRETTY_FUNCTION__ << "setting output ranks of source"
            << getClassName(src.get()) << src->objectName() << "to 0";
#endif

        for (s32 oi = 0; oi < src->getNumberOfOutputs(); oi++)
        {
            src->getOutput(oi)->setRank(0);
        }
    }

    QSet<OperatorInterface *> updated;
    QSet<OperatorInterface *> visited;

    for (auto op: m_operators)
    {
        updateRank(op.get(), updated, visited);
    }

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << "<<<<< end";
#endif
}

void Analysis::updateRank(OperatorInterface *op,
                          QSet<OperatorInterface *> &updated,
                          QSet<OperatorInterface *> &visited)
{
    assert(op);

    if (updated.contains(op))
        return;

    if (visited.contains(op))
    {
        // TODO: return error information
        qDebug() << __PRETTY_FUNCTION__ << ">>>>> ERROR: previously visited"
            << getClassName(op)
            << op->objectName();
        InvalidCodePath;
        return;
    }

    visited.insert(op);

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << ">>>>> updating rank for"
        << getClassName(op)
        << op->objectName();
#endif


    // Walk the inputs of this operator and update their ranks first
    for (s32 si = 0; si < op->getNumberOfSlots(); si++)
    {
        if (Pipe *inputPipe = op->getSlot(si)->inputPipe)
        {
            auto *inputObject(inputPipe->getSource());

            // Only operators need to be updated. Data sources will have their
            // output ranks set to 0 already.
            if (auto inputOperator = qobject_cast<OperatorInterface *>(inputObject))
            {
                updateRank(inputOperator, updated, visited);
            }
        }
    }

    const s32 maxInputRank = op->getMaximumInputRank();
    s32 newRank = maxInputRank + 1;

    // Check if the operator uses a condition and update that. Then adjust this
    // operators input rank.
    if (auto condLink = getConditionLink(op))
    {
        updateRank(condLink.condition.get(), updated, visited);

        const s32 condRank = condLink.condition->getRank();

        newRank = std::max(maxInputRank, condRank) + 1;

        qDebug() << __PRETTY_FUNCTION__ << "op" << op
            << " uses conditon" << condLink.condition.get()
            << ", this.maxInputRank =" << maxInputRank
            << ", cond.rank =" << condRank
            << ", newRank =" << newRank;
    }

    op->setRank(newRank);
    updated.insert(op);

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << "newRank =" << newRank;
#endif

    for (s32 oi = 0; oi < op->getNumberOfOutputs(); oi++)
    {
        op->getOutput(oi)->setRank(newRank);

#if ENABLE_ANALYSIS_DEBUG
        qDebug() << __PRETTY_FUNCTION__ << "output" << oi
            << "now has rank" << op->getOutput(oi)->getRank();
#endif
    }

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << "<<<<< updated rank for"
        << getClassName(op)
        << op->objectName()
        << "new rank" << op->getRank();
#endif
}

void Analysis::beginRun(const RunInfo &runInfo,
                        const vme_analysis_common::VMEIdToIndex &vmeMap,
                        Logger logger)
{
    using ClockType = std::chrono::high_resolution_clock;
    auto tStart = ClockType::now();


    const bool fullBuild = (
        m_runInfo.runId != runInfo.runId
        || m_runInfo.isReplay != runInfo.isReplay
        || m_vmeMap != vmeMap
        || getObjectFlags() & ObjectFlags::NeedsRebuild);

    qDebug() << __PRETTY_FUNCTION__
        << "fullBuild =" << fullBuild
        << ", keepAnalysisState =" << runInfo.keepAnalysisState
        << ", runId =" << runInfo.runId
        << ", localFlags =" << to_string(getObjectFlags())
        ;

    m_runInfo = runInfo;
    m_vmeMap = vmeMap;

    if (!runInfo.keepAnalysisState)
    {
        m_timetickCount = 0.0;
    }

    // Update operator ranks and then sort. This needs to be done before the a2
    // system can be built.

    updateRanks();

    qSort(m_operators.begin(), m_operators.end(),
          [] (const OperatorPtr &op1, const OperatorPtr &op2) {
        return op1->getRank() < op2->getRank();
    });

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__ << "<<<<< operators sorted by rank";
    for (const auto &op: m_operators)
    {
        qDebug() << "  "
            << "rank =" << op->getRank()
            << ", maxInputRank =" << op->getMaximumInputRank()
            << getClassName(op.get())
            << op->objectName()
            << ", max output rank =" << op->getMaximumOutputRank()
            << ", flags =" << op->getObjectFlags();
    }
    qDebug() << __PRETTY_FUNCTION__ << ">>>>> operators sorted by rank";
#endif

    qDebug() << __PRETTY_FUNCTION__ << "Analysis contains"
        << m_sources.size() << " data sources and"
        << m_operators.size() << " operators";

    u32 sourcesBuilt = 0;

    for (auto &source: m_sources)
    {
        if (fullBuild || source->getObjectFlags() & ObjectFlags::NeedsRebuild)
        {
            qDebug() << __PRETTY_FUNCTION__
                << "beginRun() on"
                << " class =" << source->metaObject()->className()
                << ", name =" << source->objectName()
                << ", id =" << source->getId()
                << ", fullBuild =" << fullBuild
                << ", objectFlags =" << to_string(source->getObjectFlags());

            source->beginRun(runInfo, logger);
            source->clearObjectFlags(ObjectFlags::NeedsRebuild);
            sourcesBuilt++;
        }
        else if (!runInfo.keepAnalysisState)
        {
            source->clearState();
        }
    }

    u32 operatorsBuilt = 0;

    for (auto &op: m_operators)
    {
        if (auto sink = qobject_cast<SinkInterface *>(op.get()))
        {
            if (!sink->isEnabled())
            {
                continue;
            }
        }

        if (fullBuild || op->getObjectFlags() & ObjectFlags::NeedsRebuild)
        {
            qDebug() << __PRETTY_FUNCTION__
                << "beginRun() on"
                << " class =" << op->metaObject()->className()
                << ", name =" << op->objectName()
                << ", id =" << op->getId()
                << ", fullBuild =" << fullBuild
                << ", objectFlags =" << to_string(op->getObjectFlags())
                << ", connected_and_valid =" << required_inputs_connected_and_valid(op.get())
                ;

            op->beginRun(runInfo, logger);
            op->clearObjectFlags(ObjectFlags::NeedsRebuild);
            operatorsBuilt++;
        }
        else if (!runInfo.keepAnalysisState)
        {
            op->clearState();
        }
    }

    clearObjectFlags(ObjectFlags::NeedsRebuild);

    qDebug() << __PRETTY_FUNCTION__ << "built" << sourcesBuilt << "sources"
        " and " << operatorsBuilt << "operators";

    // Build the a2 system

    // a2 arena swap
    m_a2ArenaIndex = (m_a2ArenaIndex + 1) % m_a2Arenas.size();
    m_a2Arenas[m_a2ArenaIndex]->reset();

    m_a2WorkArena->reset();

    qDebug() << __PRETTY_FUNCTION__ << "########## a2 active ##########";
    qDebug() << __PRETTY_FUNCTION__ << "a2: using arena" << (u32)m_a2ArenaIndex;

    m_a2State = std::make_unique<A2AdapterState>(
        a2_adapter_build_memory_wrapper(
            m_a2Arenas[m_a2ArenaIndex],
            m_a2WorkArena,
            this,
            m_sources,
            m_operators,
            m_vmeMap,
            runInfo,
            logger));

    a2::a2_begin_run(m_a2State->a2, [logger] (const std::string &str) {
        logger(QString::fromStdString(str));
    });

    auto tEnd = ClockType::now();
    std::chrono::duration<float> elapsed = tEnd - tStart;

    qDebug() << __PRETTY_FUNCTION__ << "analysis build took"
        << elapsed.count() << "seconds";
}

void Analysis::beginRun(BeginRunOption option, Logger logger)
{
    switch (option)
    {
        case BeginRunOption::ClearState:
            m_runInfo.keepAnalysisState = false;
            break;
        case BeginRunOption::KeepState:
            m_runInfo.keepAnalysisState = true;
            break;
    }

    beginRun(m_runInfo, m_vmeMap, logger);
}

void Analysis::endRun()
{
    a2::a2_end_run(m_a2State->a2);

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__
        << "calling endRun() on" << m_sources.size() << "data sources";
#endif

    for (auto &source: m_sources)
    {
        source->endRun();
    }

#if ENABLE_ANALYSIS_DEBUG
    qDebug() << __PRETTY_FUNCTION__
        << "calling endRun() on" << m_operators.size() << "operators";
#endif

    for (auto &op: m_operators)
    {
        op->endRun();
    }
}

//
// Processing
//
void Analysis::beginEvent(int eventIndex)
{
    a2_begin_event(m_a2State->a2, eventIndex);
}

void Analysis::processModuleData(int eventIndex, int moduleIndex, u32 *data, u32 size)
{
    a2_process_module_data(m_a2State->a2, eventIndex, moduleIndex, data, size);
}

void Analysis::endEvent(int eventIndex)
{
    a2_end_event(m_a2State->a2, eventIndex);
}

void Analysis::processTimetick()
{
    m_timetickCount += 1.0;
    a2_timetick(m_a2State->a2);
}

double Analysis::getTimetickCount() const
{
    return m_timetickCount;
}

//
// Serialization
//

Analysis::ReadResult Analysis::read(const QJsonObject &inputJson, VMEConfig *vmeConfig)
{
    ReadResult result = {};

    try
    {
        clear();
        auto objectStore = deserialize_objects(inputJson, vmeConfig, m_objectFactory);
        establish_connections(objectStore);

        for (const auto &obj: objectStore.sources)
            m_sources.append(obj);

        for (const auto &obj: objectStore.operators)
        {
            /* Hack fixing invalid userlevels for non-sinks. Userlevel 0 should
             * only contain data sources in the top view and sinks for raw data
             * in the bottom view. */
            if (!qobject_cast<SinkInterface *>(obj.get()) && obj->getUserLevel() == 0)
            {
                obj->setUserLevel(1);
            }
            m_operators.append(obj);
        }

        for (const auto &obj: objectStore.directories)
            m_directories.append(obj);

        m_vmeObjectSettings = objectStore.objectSettingsById;
        m_conditionLinks = objectStore.conditionLinks;
        loadDynamicProperties(objectStore.dynamicQObjectProperties, this);

        setModified(false);
    }
    catch (const std::runtime_error &e)
    {
        result.errorData["Message"] = e.what();
    }

    return result;
}

void Analysis::write(QJsonObject &json) const
{
    auto objects = getAllObjects();
    ObjectSerializerVisitor sv;
    visit_objects(objects.begin(), objects.end(), sv);

    json = sv.finalize(this);

    // VME Object Settings
    {
        QJsonObject dest;

        for (auto objectId: m_vmeObjectSettings.keys())
        {
            dest[objectId.toString()] = QJsonObject::fromVariantMap(
                m_vmeObjectSettings.value(objectId));
        }

        json["VMEObjectSettings"] = dest;
    }

    // Dynamic QObject Properties
    auto props = storeDynamicProperties(this);

    if (!props.isEmpty())
        json["properties"] = props;
}

//
// Misc
//
s32 Analysis::getNumberOfSinks() const
{
    return std::count_if(m_operators.begin(), m_operators.end(), [](const OperatorPtr &op) {
        return qobject_cast<SinkInterface *>(op.get()) != nullptr;
    });
}

size_t Analysis::getTotalSinkStorageSize() const
{
    return std::accumulate(m_operators.begin(), m_operators.end(),
                           static_cast<size_t>(0u),
                           [](size_t v, const OperatorPtr &op) {

        if (auto sink = qobject_cast<SinkInterface *>(op.get()))
            v += sink->getStorageSize();

        return v;
    });
}

s32 Analysis::getMaxUserLevel() const
{
    auto it = std::max_element(m_operators.begin(), m_operators.end(),
                               [](const auto &a, const auto &b) {
        return a->getUserLevel() < b->getUserLevel();
    });

    return (it != m_operators.end() ? (*it)->getUserLevel() : 0);
}

s32 Analysis::getMaxUserLevel(const QUuid &eventId) const
{
    auto ops = getOperators(eventId);

    auto it = std::max_element(ops.begin(), ops.end(),
                               [](const auto &a, const auto &b) {
        return a->getUserLevel() < b->getUserLevel();
    });

    return (it != ops.end() ? (*it)->getUserLevel() : 0);
}

void Analysis::clear()
{
    m_sources.clear();
    m_operators.clear();
    m_directories.clear();
    setModified();
}

bool Analysis::isEmpty() const
{
    return (m_sources.isEmpty()
            && m_operators.isEmpty()
            && m_directories.isEmpty()
            );
}


void Analysis::setModified(bool b)
{
    emit modified(b);

    if (m_modified != b)
    {
        m_modified = b;
        emit modifiedChanged(b);
    }
}

void Analysis::setVMEObjectSettings(const QUuid &objectId, const QVariantMap &settings)
{
    bool modifies = (settings != m_vmeObjectSettings.value(objectId));
    m_vmeObjectSettings.insert(objectId, settings);
    if (modifies) setModified(true);
}

QVariantMap Analysis::getVMEObjectSettings(const QUuid &objectId) const
{
    return m_vmeObjectSettings.value(objectId);
}

bool Analysis::anyObjectNeedsRebuild() const
{
    if (getObjectFlags() & ObjectFlags::NeedsRebuild)
        return true;

    for (auto &obj: getAllObjects())
    {
        if (obj->getObjectFlags() & ObjectFlags::NeedsRebuild)
            return true;
    }

    return false;
}

int Analysis::getCurrentAnalysisVersion()
{
    return CurrentAnalysisVersion;
}

static const double maxRawHistoBins = (1 << 16);

RawDataDisplay make_raw_data_display(std::shared_ptr<Extractor> extractor,
                                     double unitMin, double unitMax,
                                     const QString &xAxisTitle, const QString &unitLabel)
{
    RawDataDisplay result;
    result.extractor = extractor;

    auto objectName = extractor->objectName();
    auto extractionFilter = extractor->getFilter();

    double srcMin = 0.0;
    double srcMax = std::pow(2.0, extractionFilter.getDataBits());
    u32 histoBins = static_cast<u32>(std::min(srcMax, maxRawHistoBins));

    auto calibration = std::make_shared<CalibrationMinMax>();
    calibration->setObjectName(objectName);
    calibration->setUnitLabel(unitLabel);
    calibration->connectArrayToInputSlot(0, extractor->getOutput(0));

    const u32 addressCount = 1u << extractionFilter.getAddressBits();

    for (u32 addr = 0; addr < addressCount; ++addr)
    {
        calibration->setCalibration(addr, unitMin, unitMax);
    }

    result.calibration = calibration;

    auto rawHistoSink = std::make_shared<Histo1DSink>();
    rawHistoSink->setObjectName(QString("%1_raw").arg(objectName));
    rawHistoSink->m_bins = histoBins;
    rawHistoSink->m_xAxisTitle = xAxisTitle;
    result.rawHistoSink = rawHistoSink;

    auto calHistoSink = std::make_shared<Histo1DSink>();
    calHistoSink->setObjectName(QString("%1").arg(objectName));
    calHistoSink->m_bins = histoBins;
    calHistoSink->m_xAxisTitle = xAxisTitle;
    result.calibratedHistoSink = calHistoSink;

    rawHistoSink->connectArrayToInputSlot(0, extractor->getOutput(0));
    calHistoSink->connectArrayToInputSlot(0, calibration->getOutput(0));

    return result;
}

RawDataDisplay make_raw_data_display(const MultiWordDataFilter &extractionFilter,
                                     double unitMin, double unitMax,
                                     const QString &objectName,
                                     const QString &xAxisTitle, const QString &unitLabel)
{
    auto extractor = std::make_shared<Extractor>();
    extractor->setFilter(extractionFilter);
    extractor->setObjectName(objectName);

    return make_raw_data_display(extractor, unitMin, unitMax, xAxisTitle, unitLabel);
}

void add_raw_data_display(Analysis *analysis, const QUuid &eventId, const QUuid &moduleId,
                          const RawDataDisplay &display)
{
    analysis->addSource(eventId, moduleId, display.extractor);
    analysis->addOperator(eventId, 0, display.rawHistoSink);
    analysis->addOperator(eventId, 1, display.calibration);
    analysis->addOperator(eventId, 1, display.calibratedHistoSink);
}

void do_beginRun_forward(PipeSourceInterface *pipeSource, const RunInfo &runInfo)
{
    Q_ASSERT(pipeSource);

    qDebug() << __PRETTY_FUNCTION__ << "calling beginRun() on" << pipeSource;
    pipeSource->beginRun(runInfo);

    const s32 outputCount = pipeSource->getNumberOfOutputs();

    for (s32 outputIndex = 0;
         outputIndex < outputCount;
         ++outputIndex)
    {
        Pipe *outPipe = pipeSource->getOutput(outputIndex);
        Q_ASSERT(outPipe); // Must exist as the source said it would exist.

        // Copy destinations vector here as disconnectPipe() below will modify it.
        auto destinations = outPipe->destinations;
        const s32 destCount = destinations.size();

        for (s32 destIndex = 0;
             destIndex < destCount;
             ++destIndex)
        {
            Slot *destSlot = destinations[destIndex];

            if (destSlot)
            {
                Q_ASSERT(destSlot->parentOperator);

                auto destOperator = destSlot->parentOperator;

                // Check all the slots of the destination operator. This is
                // necessary as multiple slots might be connected to the
                // current pipeSource.
                for (s32 destSlotIndex = 0;
                     destSlotIndex < destOperator->getNumberOfSlots();
                     ++destSlotIndex)
                {
                    Slot *parentSlot = destOperator->getSlot(destSlotIndex);

                    if (parentSlot->inputPipe
                        && parentSlot->paramIndex != Slot::NoParamIndex
                        && parentSlot->paramIndex >= parentSlot->inputPipe->parameters.size())
                    {
                        // The paramIndex is out of range. This can happen when
                        // e.g. an Extractor is edited and the new number of
                        // address bits is less than the old number.
                        // For now handle this case by just disconnecting the slot.
                        // It will then be highlighted in the UI.
                        qDebug() << __PRETTY_FUNCTION__
                            << "disconnecting Slot" << parentSlot->name << "of operator" << destOperator
                            << "because its paramIndex is now out of range";
                        parentSlot->disconnectPipe();
                    }
                }

                do_beginRun_forward(destSlot->parentOperator);
            }
        }
    }
}

QString make_unique_operator_name(Analysis *analysis, const QString &prefix)
{
    int suffixNumber = 0;

    for (const auto &op: analysis->getOperators())
    {
        auto name = op->objectName();

        if (name.startsWith(prefix))
        {
            QString suffix;
            if (name.size() - 1 > prefix.size())
            {
                suffix = name.right(name.size() - prefix.size() - 1);
            }

            if (suffix.size())
            {
                bool ok;
                int n = suffix.toInt(&ok);
                if (ok)
                {
                    suffixNumber = std::max(suffixNumber, n);
                }
            }
        }
    }

    ++suffixNumber;

    return prefix + QSL(".") + QString::number(suffixNumber);
}

bool required_inputs_connected_and_valid(OperatorInterface *op)
{
    bool result = true;
    bool oneNonOptionalSlotConnected = false;

    for (s32 slotIndex = 0;
         slotIndex < op->getNumberOfSlots();
         ++slotIndex)
    {
        auto slot = op->getSlot(slotIndex);

        if (slot->isParamIndexInRange())
        {
            result = result && true;

            if (!slot->isOptional)
                oneNonOptionalSlotConnected = true;
        }
        else if (slot->isOptional && !slot->isConnected())
        {
            result = result && true;
        }
        else
        {
            result = false;
        }
    }

    result = result && oneNonOptionalSlotConnected;

    return result;
}

bool no_input_connected(OperatorInterface *op)
{
    bool result = true;

    for (s32 slotIndex = 0;
         slotIndex < op->getNumberOfSlots();
         ++slotIndex)
    {
        result = result && !op->getSlot(slotIndex)->isConnected();
    }

    return result;
}

QString info_string(const Analysis *analysis)
{
    QString result = QString("Analysis: %1 Data Sources, %2 Operators")
        .arg(analysis->getNumberOfSources())
        .arg(analysis->getNumberOfOperators());

    return result;
}

namespace
{

void adjust_userlevel_forward(const OperatorVector &operators,
                              OperatorInterface *op,
                              s32 levelDelta,
                              QSet<OperatorInterface *> &adjusted)
{
    // Note: Could be optimized by searching from the last operators position
    // forward if the vector is sorted by operator rank:
    // adjust_userlevel_forward(entryIt, endIt, op, ...)

    if (adjusted.contains(op) || levelDelta == 0)
        return;

    auto opit = std::find_if(operators.begin(), operators.end(),
                           [op] (const OperatorPtr &op_) {
        return op_.get() == op;
    });

    if (opit != operators.end())
    {
        auto op = *opit;

        op->setUserLevel(op->getUserLevel() + levelDelta);
        adjusted.insert(op.get());

        for (s32 oi = 0; oi < op->getNumberOfOutputs(); oi++)
        {
            auto outputPipe = op->getOutput(oi);

            for (Slot *destSlot: outputPipe->getDestinations())
            {
                if (destSlot->parentOperator)
                {
                    adjust_userlevel_forward(operators,
                                             destSlot->parentOperator,
                                             levelDelta,
                                             adjusted);
                }
            }
        }
    }
}

} // end anon namespace

void adjust_userlevel_forward(const OperatorVector &operators,
                              OperatorInterface *op,
                              s32 levelDelta)
{
    QSet<OperatorInterface *> adjusted;

    adjust_userlevel_forward(operators, op, levelDelta, adjusted);
}

namespace
{

    void expand_objects(const AnalysisObjectVector &vec,
                        const Analysis *analysis,
                        AnalysisObjectSet &visited,
                        AnalysisObjectVector &result)
{
    for (const auto &obj: vec)
    {
        if (!visited.contains(obj))
        {
            visited.insert(obj);
            result.push_back(obj);
        }

        if (auto dir = std::dynamic_pointer_cast<Directory>(obj))
        {
            expand_objects(analysis->getDirectoryContents(dir),
                           analysis, visited, result);
        }
    }
}

} // end anon namespace

AnalysisObjectVector expand_objects(const AnalysisObjectVector &vec,
                                    const Analysis *analysis)
{
    AnalysisObjectVector result;
    AnalysisObjectSet visited;

    expand_objects(vec, analysis, visited, result);

    return result;
}

AnalysisObjectVector order_objects(const AnalysisObjectSet &objects,
                                   const Analysis *analysis)
{
    AnalysisObjectVector result;

    // Iteration done in analysis object storage order.
    for (const auto &obj: analysis->getAllObjects())
    {
        if (objects.contains(obj))
            result.push_back(obj);
    }

    return result;
}

AnalysisObjectVector order_objects(const AnalysisObjectVector &objects,
                                   const Analysis *analysis)
{
    AnalysisObjectSet set;

    for (const auto &obj: objects)
        set.insert(obj);

    return order_objects(set, analysis);
}

} // end namespace analysis
