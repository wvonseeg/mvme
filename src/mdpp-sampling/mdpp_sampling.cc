#include "mdpp_sampling_p.h"

#include <set>
#include <qwt_symbol.h>
#include <QCheckBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <mesytec-mvlc/cpp_compat.h>
#include <mesytec-mvlc/util/data_filter.h>
#include <mesytec-mvlc/util/logging.h>
#include <mesytec-mvlc/util/ticketmutex.h>

#include "analysis_service_provider.h"
#include "analysis/analysis.h"
#include "analysis/a2/a2_support.h"
#include "mdpp-sampling/waveform_interpolation.h"
#include "run_info.h"
#include "util/qt_container.h"
#include "util/qt_logview.h"
#include "vme_config.h"

using namespace mesytec::mvlc;

namespace mesytec::mvme::mdpp_sampling
{

struct MdppSamplingConsumer::Private
{
    std::shared_ptr<spdlog::logger> logger_;
    Logger qtLogger_;
    mesytec::mvlc::TicketMutex mutex_;
    std::set<QUuid> moduleInterests_;
    vme_analysis_common::VMEIdToIndex vmeIdToIndex_;
    vme_analysis_common::IndexToVmeId indexToVmeId_;
    std::array<size_t, MaxVMEEvents> linearEventNumbers_;

    // This data obtained in beginRun()
    RunInfo runInfo_;
    const VMEConfig *vmeConfig_ = nullptr;
    analysis::Analysis *analysis_ = nullptr;


    bool hasModuleInterest(s32 crateIndex, s32 eventIndex, s32 moduleIndex)
    {
        // TODO: crateIndex needs to be checked if this should be multicrate capable.

        auto guard = std::unique_lock(mutex_);
        // Map indexes back to the moduleId, then check if it's present in the
        // moduleInterests_ set.
        auto moduleId = indexToVmeId_.value({ eventIndex, moduleIndex});
        return moduleInterests_.find(moduleId) != std::end(moduleInterests_);
    }
};

MdppSamplingConsumer::MdppSamplingConsumer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->logger_ = get_logger("MdppSamplingConsumer");
    d->logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [tid%t] %v");
    d->logger_->set_level(spdlog::level::debug);
    d->linearEventNumbers_.fill(0);
}

MdppSamplingConsumer::~MdppSamplingConsumer()
{
}

void MdppSamplingConsumer::setLogger(Logger logger)
{
    d->qtLogger_ = logger;
}

StreamConsumerBase::Logger &MdppSamplingConsumer::getLogger()
{
    return d->qtLogger_;
}

void MdppSamplingConsumer::beginRun(
    const RunInfo &runInfo, const VMEConfig *vmeConfig, analysis::Analysis *analysis)
{
    d->runInfo_ = runInfo;
    d->vmeConfig_ = vmeConfig;
    d->analysis_ = analysis;
    d->vmeIdToIndex_ = analysis->getVMEIdToIndexMapping();
    d->indexToVmeId_ = reverse_hash(d->vmeIdToIndex_);
    std::fill(std::begin(d->linearEventNumbers_), std::end(d->linearEventNumbers_), 0);
    emit sigBeginRun(runInfo, vmeConfig, analysis);
}

void MdppSamplingConsumer::endRun(const DAQStats &stats, const std::exception *e)
{
    emit sigEndRun(stats, e);
}

void MdppSamplingConsumer::beginEvent(s32 eventIndex)
{
    (void) eventIndex;
}

void MdppSamplingConsumer::endEvent(s32 eventIndex)
{
    (void) eventIndex;
}

void MdppSamplingConsumer::processModuleData(
    s32 crateIndex, s32 eventIndex, const ModuleData *moduleDataList, unsigned moduleCount)
{
    assert(0 <= eventIndex && static_cast<u32>(eventIndex) < d->linearEventNumbers_.size());

    ++d->linearEventNumbers_[eventIndex];

    for (unsigned moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex)
    {
        if (d->hasModuleInterest(crateIndex, eventIndex, moduleIndex))
        {
            auto dataBlock = moduleDataList[moduleIndex].data;
            std::vector<u32> buffer(dataBlock.size);
            std::copy(dataBlock.data, dataBlock.data+dataBlock.size, std::begin(buffer));
            vme_analysis_common::VMEConfigIndex vmeIndex{eventIndex, static_cast<s32>(moduleIndex)};
            auto moduleId = d->indexToVmeId_.value(vmeIndex);
            emit moduleDataReady(moduleId, buffer, d->linearEventNumbers_[eventIndex]);
        }
    }
}

void MdppSamplingConsumer::processSystemEvent(s32 crateIndex, const u32 *header, u32 size)
{
    (void) crateIndex;
    (void) header;
    (void) size;
}

void MdppSamplingConsumer::processModuleData(s32 eventIndex, s32 moduleIndex, const u32 *data, u32 size)
{
    (void) eventIndex;
    (void) moduleIndex;
    (void) data;
    (void) size;
    assert(!"don't call me please!");
    throw std::runtime_error(fmt::format("{}: don't call me please!", __PRETTY_FUNCTION__));
}

void MdppSamplingConsumer::addModuleInterest(const QUuid &moduleId)
{
    auto guard = std::unique_lock(d->mutex_);
    d->moduleInterests_.insert(moduleId);
}

inline MdppChannelTracePlotData *get_curve_data(QwtPlotCurve *curve)
{
    return reinterpret_cast<MdppChannelTracePlotData *>(curve->data());
}

struct TracePlotWidget::Private
{
    TracePlotWidget *q = nullptr;
    QwtPlotCurve *rawCurve_ = nullptr;                  // curve for the raw samples
    QwtPlotCurve *interpolatedCurve_ = nullptr;         // 2nd curve for the interpolated samples
    MdppChannelTracePlotData *rawCurveData_ = nullptr;     // has to be a ptr as qwt takes ownership and deletes it. more needed for multi plots
    MdppChannelTracePlotData *interpolatedCurveData_ = nullptr;  // ownership goes to qwt when it's attached to
    QwtPlotZoomer *zoomer_ = nullptr;
};

TracePlotWidget::TracePlotWidget(QWidget *parent)
    : histo_ui::PlotWidget(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
    d->rawCurve_ = new QwtPlotCurve("rawCurve");
    d->interpolatedCurve_ = new QwtPlotCurve("interpolatedCurve");

    for (auto curve: { d->rawCurve_, d->interpolatedCurve_ })
    {
        curve->setPen(Qt::black);
        curve->setRenderHint(QwtPlotItem::RenderAntialiased);
        curve->attach(getPlot());
    }

    // raw samples: no lines, just a marker at each raw sample coordinate
    d->rawCurveData_ = new MdppChannelTracePlotData();
    d->rawCurveData_->setMode(PlotDataMode::Raw);
    d->rawCurve_->setStyle(QwtPlotCurve::NoCurve);
    d->rawCurve_->setData(d->rawCurveData_);

    auto crossSymbol = new QwtSymbol(QwtSymbol::Diamond);
    crossSymbol->setSize(QSize(5, 5));
    crossSymbol->setColor(Qt::red);
    d->rawCurve_->setSymbol(crossSymbol);

    // interpolated samples: lines from point to point, no markers
    d->interpolatedCurveData_ = new MdppChannelTracePlotData();
    d->interpolatedCurveData_->setMode(PlotDataMode::Interpolated);
    d->interpolatedCurve_->setData(d->interpolatedCurveData_);
    d->interpolatedCurve_->setStyle(QwtPlotCurve::Lines);

    getPlot()->axisWidget(QwtPlot::xBottom)->setTitle("Time [ns]");
    histo_ui::setup_axis_scale_changer(this, QwtPlot::yLeft, "Y-Scale");
    d->zoomer_ = histo_ui::install_scrollzoomer(this);
    histo_ui::install_tracker_picker(this);

    DO_AND_ASSERT(connect(d->zoomer_, SIGNAL(zoomed(const QRectF &)), this, SLOT(replot())));

    // enable both the zoomer and mouse cursor tracker by default

    if (auto zoomAction = findChild<QAction *>("zoomAction"))
        zoomAction->setChecked(true);

    if (auto trackerPickerAction = findChild<QAction *>("trackerPickerAction"))
        trackerPickerAction->setChecked(true);

    auto tb = getToolBar();
    tb->addSeparator();

}

TracePlotWidget::~TracePlotWidget()
{
}

QRectF TracePlotWidget::traceBoundingRect() const
{
    return d->interpolatedCurveData_->boundingRect();
}

QwtPlotCurve *TracePlotWidget::getRawCurve()
{
    return d->rawCurve_;
}

QwtPlotCurve *TracePlotWidget::getInterpolatedCurve()
{
    return d->interpolatedCurve_;
}

void TracePlotWidget::setTrace(const ChannelTrace *trace)
{
    if (trace != d->rawCurveData_->getTrace())
    {
        d->rawCurveData_->setTrace(trace);
        d->interpolatedCurveData_->setTrace(trace);
    }
}

const ChannelTrace *TracePlotWidget::getTrace() const
{
    return d->rawCurveData_->getTrace();
}

static const int ReplotInterval_ms = 33;
static const size_t TraceHistoryMaxDepth = 10 * 1000;

struct MdppSamplingUi::Private
{
    MdppSamplingUi *q = nullptr;
    AnalysisServiceProvider *asp_ = nullptr;
    TracePlotWidget *plotWidget_ = nullptr;
    QTimer replotTimer_;
    DecodedMdppSampleEvent currentEvent_;
    QComboBox *moduleSelect_ = nullptr;
    QSpinBox *channelSelect_ = nullptr;
    QSpinBox *traceSelect_ = nullptr;
    TraceHistoryMap traceHistory_;
    size_t traceHistoryMaxDepth = 10;
    QPushButton *pb_printInfo_ = nullptr;
    QDoubleSpinBox *spin_dtSample_ = nullptr;
    QSpinBox *spin_interpolationFactor_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QCheckBox *cb_sampleSymbols_ = nullptr;
    QCheckBox *cb_interpolatedSymbols_ = nullptr;
    WidgetGeometrySaver *geoSaver_ = nullptr;
    QRectF maxBoundingRect_;
    QPushButton *pb_resetBoundingRect = nullptr;

    void printInfo();
    void updatePlotAxisScales();
};

MdppSamplingUi::MdppSamplingUi(AnalysisServiceProvider *asp, QWidget *parent)
    : histo_ui::IPlotWidget(parent)
    , d(std::make_unique<Private>())
{
    qRegisterMetaType<DecodedMdppSampleEvent>("mesytec::mvme::DecodedMdppSampleEvent");
    qRegisterMetaType<size_t>("size_t");
    qRegisterMetaType<RunInfo>("RunInfo");

    setWindowTitle("MDPP Sampling Mode: Trace Browser");

    d->q = this;
    d->asp_ = asp;
    d->plotWidget_ = new TracePlotWidget(this);
    d->replotTimer_.setInterval(ReplotInterval_ms);
    connect(&d->replotTimer_, &QTimer::timeout, this, &MdppSamplingUi::replot);
    d->replotTimer_.start();

    auto l = make_hbox<0, 0>(this);
    l->addWidget(d->plotWidget_);

    auto tb = getToolBar();

    {
        d->moduleSelect_ = new QComboBox;
        d->moduleSelect_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        auto boxStruct = make_vbox_container(QSL("Module"), d->moduleSelect_, 0, -2);
        tb->addWidget(boxStruct.container.release());
    }

    tb->addSeparator();

    {
        d->channelSelect_ = new QSpinBox;
        d->channelSelect_->setMinimum(0);
        d->channelSelect_->setMaximum(0);
        auto boxStruct = make_vbox_container(QSL("Channel"), d->channelSelect_, 0, -2);
        tb->addWidget(boxStruct.container.release());
    }

    tb->addSeparator();

    {
        d->traceSelect_ = new QSpinBox;
        d->traceSelect_->setMinimum(0);
        d->traceSelect_->setMaximum(1);
        d->traceSelect_->setSpecialValueText("latest");
        auto boxStruct = make_vbox_container(QSL("Trace#"), d->traceSelect_, 0, -2);
        tb->addWidget(boxStruct.container.release());
    }

    tb->addSeparator();

    {
        d->spin_dtSample_ = new QDoubleSpinBox;
        d->spin_dtSample_->setMinimum(1.0);
        d->spin_dtSample_->setMaximum(1e9);
        d->spin_dtSample_->setSingleStep(0.1);
        d->spin_dtSample_->setSuffix(" ns");
        d->spin_dtSample_->setValue(MdppDefaultSamplePeriod);

        auto pb_useDefaultSampleInterval = new QPushButton(QIcon(":/reset_to_default.png"), {});

        connect(pb_useDefaultSampleInterval, &QPushButton::clicked, this, [this] {
            d->spin_dtSample_->setValue(MdppDefaultSamplePeriod);
        });

        auto [w0, l0] = make_widget_with_layout<QWidget, QHBoxLayout>();
        l0->addWidget(d->spin_dtSample_);
        l0->addWidget(pb_useDefaultSampleInterval);

        auto boxStruct = make_vbox_container(QSL("Sample Interval"), w0, 0, -2);
        tb->addWidget(boxStruct.container.release());
    }

    tb->addSeparator();

    {
        d->spin_interpolationFactor_ = new QSpinBox;
        d->spin_interpolationFactor_->setSpecialValueText("off");
        d->spin_interpolationFactor_->setMinimum(0);
        d->spin_interpolationFactor_->setMaximum(100);
        d->spin_interpolationFactor_->setValue(5);
        auto boxStruct = make_vbox_container(QSL("Interpolation Factor"), d->spin_interpolationFactor_, 0, -2);
        tb->addWidget(boxStruct.container.release());
    }

    tb->addSeparator();

    {
        d->cb_sampleSymbols_ = new QCheckBox("Sample Symbols");
        d->cb_sampleSymbols_->setChecked(true);
        d->cb_interpolatedSymbols_ = new QCheckBox("Interpolated Symbols");
        d->pb_resetBoundingRect = new QPushButton("Reset Axis Scales");
        auto [widget, layout] = make_widget_with_layout<QWidget, QVBoxLayout>();
        layout->addWidget(d->cb_sampleSymbols_);
        layout->addWidget(d->cb_interpolatedSymbols_);
        layout->addWidget(d->pb_resetBoundingRect);
        tb->addWidget(widget);

        connect(d->pb_resetBoundingRect, &QPushButton::clicked, this, [this] {
            d->maxBoundingRect_ = {};
        });
    }

    tb->addSeparator();

    d->pb_printInfo_ = new QPushButton("Print Info");
    tb->addWidget(d->pb_printInfo_);

    connect(d->pb_printInfo_, &QPushButton::clicked, this, [this] { d->printInfo(); });

    connect(d->moduleSelect_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MdppSamplingUi::replot);
    connect(d->channelSelect_, qOverload<int>(&QSpinBox::valueChanged), this, &MdppSamplingUi::replot);
    connect(d->traceSelect_, qOverload<int>(&QSpinBox::valueChanged), this, &MdppSamplingUi::replot);

    d->geoSaver_ = new WidgetGeometrySaver(this);
    d->geoSaver_->addAndRestore(this, "WindowGeometries/MdppSamplingUi");
}

MdppSamplingUi::~MdppSamplingUi() { }

QwtPlot *MdppSamplingUi::getPlot()
{
    return d->plotWidget_->getPlot();
}

const QwtPlot *MdppSamplingUi::getPlot() const
{
    return d->plotWidget_->getPlot();
}

QToolBar *MdppSamplingUi::getToolBar()
{
    return d->plotWidget_->getToolBar();
}

QStatusBar *MdppSamplingUi::getStatusBar()
{
    return d->plotWidget_->getStatusBar();
}

s32 get_max_channel_number(const DecodedMdppSampleEvent &event)
{
    s32 ret = -1;

    for (const auto &trace: event.traces)
        ret = std::max(ret, trace.channel);

    return ret;
}

s32 get_max_depth(const ModuleTraceHistory &history)
{
    s32 ret = 0;

    for (const auto &traceBuffer: history)
        ret = std::max(ret, traceBuffer.size());

    return ret;
}

void MdppSamplingUi::updateUi()
{
    spdlog::trace("begin MdppSamplingUi::updateUi()");

    // selector 1: Add missing modules to the module select combo
    {
        QSet<QUuid> knownModuleIds;

        for (int i=0; i<d->moduleSelect_->count(); ++i)
        {
            auto moduleId = d->moduleSelect_->itemData(i).value<QUuid>();
            knownModuleIds.insert(moduleId);
        }

        QSet<QUuid> traceModuleIds = QSet<QUuid>::fromList(d->traceHistory_.keys());

        //qDebug() << "knownModuleIds" << knownModuleIds;
        //qDebug() << "traceModuleIds" << traceModuleIds;

        auto missingModuleIds = traceModuleIds.subtract(knownModuleIds);

        //qDebug() << "missingModuleIds" << missingModuleIds;

        for (auto moduleId: missingModuleIds)
        {
            QString moduleName;

            if (auto moduleConfig = d->asp_->getVMEConfig()->getModuleConfig(moduleId))
                moduleName = moduleConfig->getObjectPath();
            else
                moduleName = moduleId.toString();

            d->moduleSelect_->addItem(moduleName, moduleId);
        }
    }

    // selector 2: Update the channel number spinbox
    if (auto selectedModuleId = d->moduleSelect_->currentData().value<QUuid>();
        !selectedModuleId.isNull())
    {
        auto &moduleTraceHistory = d->traceHistory_[selectedModuleId];
        const auto maxChannel = static_cast<signed>(moduleTraceHistory.size()) - 1;
        d->channelSelect_->setMaximum(std::max(maxChannel, d->channelSelect_->maximum()));
        auto selectedChannel = d->channelSelect_->value();

        // selector 3: trace number in the trace history. index 0 is the latest trace.
        if (0 <= selectedChannel && selectedChannel <= maxChannel)
        {
            auto &tracebuffer = moduleTraceHistory[selectedChannel];
            d->traceSelect_->setMaximum(std::max(1, tracebuffer.size()-1));
        }
        else
        {
            d->traceSelect_->setMaximum(0);
        }
    }
    else // no module selected
    {
        d->channelSelect_->setMaximum(1);
        d->traceSelect_->setMaximum(1);
    }

    // add / remove symbols from / to the curves

    if (auto curve = d->plotWidget_->getRawCurve();
        curve && !d->cb_sampleSymbols_->isChecked())
    {
        curve->setSymbol(nullptr);
    }
    else if (curve && !curve->symbol())
    {
        auto crossSymbol = new QwtSymbol(QwtSymbol::Diamond);
        crossSymbol->setSize(QSize(5, 5));
        crossSymbol->setColor(Qt::red);
        curve->setSymbol(crossSymbol);
    }

    if (auto curve = d->plotWidget_->getInterpolatedCurve();
        curve && !d->cb_interpolatedSymbols_->isChecked())
    {
        curve->setSymbol(nullptr);
    }
    else if (curve && !curve->symbol())
    {
        auto crossSymbol = new QwtSymbol(QwtSymbol::Triangle);
        crossSymbol->setSize(QSize(5, 5));
        crossSymbol->setColor(Qt::blue);
        curve->setSymbol(crossSymbol);
    }

    spdlog::trace("end MdppSamplingUi::updateUi()");
}

void MdppSamplingUi::replot()
{
    spdlog::trace("begin MdppSamplingUi::replot()");

    updateUi(); // update, selection boxes, buttons, etc.

    auto selectedModuleId = d->moduleSelect_->currentData().value<QUuid>();
    ChannelTrace *trace = nullptr;

    if (d->traceHistory_.contains(selectedModuleId))
    {
        auto &moduleTraceHistory = d->traceHistory_[selectedModuleId];
        const auto channelCount = moduleTraceHistory.size();
        const auto selectedChannel = d->channelSelect_->value();

        if (0 <= selectedChannel && static_cast<size_t>(selectedChannel) < channelCount)
        {
            auto &tracebuffer = moduleTraceHistory[selectedChannel];

            // selector 3: trace number in the trace history. 0 is the latest trace.
            auto selectedTraceIndex = d->traceSelect_->value();

            if (0 <= selectedTraceIndex && selectedTraceIndex < tracebuffer.size())
                trace = &tracebuffer[selectedTraceIndex];
        }
    }

    if (trace)
    {
        auto interpolationFactor = 1+ d->spin_interpolationFactor_->value();
        auto dtSample = d->spin_dtSample_->value();

        trace->dtSample = dtSample;
        interpolate(*trace, interpolationFactor);
    }

    d->plotWidget_->setTrace(trace);
    d->updatePlotAxisScales();
    d->plotWidget_->replot();

    auto sb = getStatusBar();
    sb->clearMessage();

    if (trace)
    {
        if (auto mod = d->asp_->getVMEConfig()->getModuleConfig(trace->moduleId))
        {
            auto moduleName = mod->getObjectPath();
            auto channel = trace->channel;
            auto traceIndex = d->traceSelect_->value();
            auto boundingRect = d->plotWidget_->traceBoundingRect();
            double yMin = boundingRect.bottom();
            double yMax = boundingRect.top();

            sb->showMessage(QSL("Module: %1, Channel: %2, Trace: %3, Event#: %4, #RawSamples=%5, #IpolSamples=%6 yMin=%7, yMax=%8, moduleHeader=0x%9")
                .arg(moduleName).arg(channel).arg(traceIndex).arg(trace->eventNumber)
                .arg(get_raw_sample_count(*trace))
                .arg(get_interpolated_sample_count(*trace))
                .arg(yMin).arg(yMax).arg(trace->moduleHeader, 8, 16, QLatin1Char('0'))
                );
        }
    }

    spdlog::trace("end MdppSamplingUi::replot()");
}

void MdppSamplingUi::beginRun(const RunInfo &runInfo, const VMEConfig *vmeConfig, const analysis::Analysis *analysis)
{
    (void) runInfo;
    (void) vmeConfig;
    (void) analysis;

    spdlog::info("MdppSamplingUi::beginRun()");

    d->maxBoundingRect_ = {};
    d->plotWidget_->setTrace(nullptr);
    d->traceHistory_.clear();

    replot();
}

void MdppSamplingUi::handleModuleData(const QUuid &moduleId, const std::vector<u32> &buffer, size_t linearEventNumber)
{
    spdlog::trace("MdppSamplingUi::handleModuleData event#{}, moduleId={}, size={}",
        linearEventNumber, moduleId.toString().toLocal8Bit().data(), buffer.size());

    auto moduleTypename = d->asp_->getVMEConfig()->getModuleConfig(moduleId)->getModuleMeta().typeName; // slow and dangerous, that's how we roll

    auto decodedEvent = decode_mdpp_samples(buffer.data(), buffer.size(), moduleTypename.toLocal8Bit().constData());
    decodedEvent.eventNumber = linearEventNumber;
    decodedEvent.moduleId = moduleId;

    for (auto &trace: decodedEvent.traces)
    {
        // fill in missing info (decode_mdpp_samples() does not know this stuff)
        trace.eventNumber = linearEventNumber;
        trace.moduleId = moduleId;
    }

    for (const auto &trace: decodedEvent.traces)
    {
        spdlog::trace("MdppSamplingUi::handleModuleData event#{}, got a trace for channel {} containing {} samples",
                    trace.eventNumber, trace.channel, trace.samples.size());
    }

    // Add the events traces to the trace history.

    auto it_maxChan = std::max_element(std::begin(decodedEvent.traces), std::end(decodedEvent.traces),
        [](const auto &lhs, const auto &rhs) { return lhs.channel < rhs.channel; });

    // No traces in the event, nothing to do.
    if (it_maxChan == std::end(decodedEvent.traces))
        return;

    auto &moduleTraceHistory = d->traceHistory_[moduleId];
    moduleTraceHistory.resize(std::max(moduleTraceHistory.size(), static_cast<size_t>(it_maxChan->channel)+1));

    for (const auto &trace: decodedEvent.traces)
    {
        auto &traceBuffer = moduleTraceHistory[trace.channel];
        traceBuffer.push_front(trace); // prepend the trace -> newest trace is always at the front
        // remove old traces
        while (static_cast<size_t>(traceBuffer.size()) > TraceHistoryMaxDepth)
            traceBuffer.pop_back();
    }
}

void MdppSamplingUi::endRun(const DAQStats &stats, const std::exception *e)
{
    (void) stats;
    (void) e;
    spdlog::info("MdppSamplingUi::endRun()");
}

void MdppSamplingUi::addModuleInterest(const QUuid &moduleId)
{
    emit moduleInterestAdded(moduleId);
}

void MdppSamplingUi::Private::printInfo()
{
    auto trace = plotWidget_->getTrace();

    if (!trace)
        return;

    QString text;
    QTextStream out(&text);

    auto moduleName = asp_->getVMEConfig()->getModuleConfig(trace->moduleId)->getObjectPath();
    auto moduleTypename = asp_->getVMEConfig()->getModuleConfig(trace->moduleId)->getModuleMeta().typeName;

    out << QSL("module %1 (type=%2)\n").arg(moduleName).arg(moduleTypename);
    out << fmt::format("linear event number: {}\n", trace->eventNumber).c_str();
    out << fmt::format("sampleCount: {}\n", trace->samples.size()).c_str();
    out << fmt::format("samples: [{}]\n", fmt::join(trace->samples, ", ")).c_str();
    out << fmt::format("interpolated: [{}]\n", fmt::join(trace->interpolated, ", ")).c_str();
    out << fmt::format("interpolatedCount: {}\n", trace->interpolated.size()).c_str();
    out << fmt::format("dtSample: {}\n", trace->dtSample).c_str();

    if (!logView_)
    {
        logView_ = make_logview().release();
        logView_->setWindowTitle("MDPP Sampling Mode: Trace Info");
        logView_->setAttribute(Qt::WA_DeleteOnClose);
        logView_->resize(1000, 600);
        connect(logView_, &QWidget::destroyed, q, [this] { logView_ = nullptr; });
        add_widget_close_action(logView_);
        geoSaver_->addAndRestore(logView_, "WindowGeometries/MdppSamplingUiLogView");
    }

    assert(logView_);
    logView_->setPlainText(text);
    logView_->show();
    logView_->raise();
}

void MdppSamplingUi::Private::updatePlotAxisScales()
{
    spdlog::trace("entering MdppSamplingUi::Private::updatePlotAxisScales()");

    auto plot = plotWidget_->getPlot();
    auto zoomer = histo_ui::get_zoomer(plotWidget_);
    assert(plot && zoomer);

    // Grow the bounding rect to the max of every trace seen in this run to keep
    // the display from jumping when switching between traces.
    auto newBoundingRect = maxBoundingRect_;

    if (!newBoundingRect.isValid())
    {
        newBoundingRect = plotWidget_->traceBoundingRect();
        auto xMin = newBoundingRect.left();
        auto xMax = newBoundingRect.right();
        auto yMax = newBoundingRect.bottom();
        auto yMin = newBoundingRect.top();
        spdlog::trace("updatePlotAxisScales(): setting initial bounding rect from trace: xMin={}, xMax={}, yMin={}, yMax={}", xMin, xMax, yMin, yMax);
    }

    newBoundingRect = newBoundingRect.united(plotWidget_->traceBoundingRect());

    spdlog::trace("updatePlotAxisScales(): zoomRectIndex()={}", zoomer->zoomRectIndex());

    if (!maxBoundingRect_.isValid() || zoomer->zoomRectIndex() == 0)
    {
        auto xMin = newBoundingRect.left();
        auto xMax = newBoundingRect.right();
        auto yMax = newBoundingRect.bottom();
        auto yMin = newBoundingRect.top();

        spdlog::trace("updatePlotAxisScales(): updatePlotAxisScales(): forcing axis scales to: xMin={}, xMax={}, yMin={}, yMax={}", xMin, xMax, yMin, yMax);

        plot->setAxisScale(QwtPlot::xBottom, xMin, xMax);
        histo_ui::adjust_y_axis_scale(plot, yMin, yMax);
        plot->updateAxes();

        if (zoomer->zoomRectIndex() == 0)
        {
            spdlog::trace("updatePlotAxisScales(): zoomer fully zoomed out -> setZoomBase()");
            zoomer->setZoomBase();
        }
    }

    maxBoundingRect_ = newBoundingRect;
}

QVector<std::pair<double, double>> interpolate(const mvlc::basic_string_view<s16> &samples, u32 factor,
    double dtSample)
{
    QVector<std::pair<double, double>> result;

    mesytec::mvme::waveforms::interpolate({ samples.data(), samples.size() }, dtSample, factor,
        [&result](double x, double y)
    {
            result.push_back({ x, y });
    });

    return result;
}

}
