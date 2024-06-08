#include <argh.h>
#include <mesytec-mvlc/mesytec-mvlc.h>
#include <QApplication>
#include <QCheckBox>
#include <QMainWindow>
#include <QMenuBar>
#include <QPushButton>
#include <QTimer>

#include "analysis/analysis_ui.h"
#include "analysis/analysis_util.h"
#include "multi_crate.h"
#include "multi_crate_nng.h"
#include "multi_crate_nng_gui.h"
#include "mvlc_daq.h"
#include "mvlc/vmeconfig_to_crateconfig.h"
#include "mvme_session.h"
#include "qt_util.h"
#include "util/qt_monospace_textedit.h"
#include "util/signal_handling.h"
#include "util/stopwatch.h"
#include "vme_config_util.h"
#include "vme_analysis_common.h"

using namespace mesytec;
using namespace mesytec::mvlc;
using namespace mesytec::mvme;
using namespace mesytec::mvme::multi_crate;

int main(int argc, char *argv[])
{
    std::string generalHelp = R"~(
        write the help text please!
)~";

    QApplication app(argc, argv);
    mvme_init("mvme_multicrate_replay", false);

    app.setWindowIcon(QIcon(":/window_icon.png"));

    spdlog::set_level(spdlog::level::warn);
    mesytec::mvlc::set_global_log_level(spdlog::level::warn);

    setup_signal_handlers();

    if (argc < 2)
    {
        std::cout << generalHelp;
        return 1;
    }

    argh::parser parser({"-h", "--help", "--log-level"});
    parser.add_params({"--analysis"});
    parser.parse(argv);

    {
        std::string logLevelName;
        if (parser("--log-level") >> logLevelName)
            logLevelName = str_tolower(logLevelName);
        else if (parser["--trace"])
            logLevelName = "trace";
        else if (parser["--debug"])
            logLevelName = "debug";
        else if (parser["--info"])
            logLevelName = "info";

        if (!logLevelName.empty())
            spdlog::set_level(spdlog::level::from_str(logLevelName));
        else
            spdlog::set_level(spdlog::level::info);
    }

    trace_log_parser_info(parser, "mvme_multicrate_replay");

    if (parser.pos_args().size() <= 1)
    {
        std::cerr << "Error: no listfile filename given on command line\n";
        return 1;
    }

    std::string listfileFilename = parser.pos_args()[1];
    std::string analysisFilename;
    parser("--analysis") >> analysisFilename;

    mvlc::listfile::SplitZipReader zipReader;
    try
    {
        zipReader.openArchive(listfileFilename);
    }
    catch(const std::exception& e)
    {
        std::cerr << fmt::format("Could not open listfile archive {}: {}\n", listfileFilename, e.what());
        return 1;
    }

    auto listfileEntryName = zipReader.firstListfileEntryName();

    if (listfileEntryName.empty())
    {
        std::cerr << "Error: no listfile entry found in " << listfileFilename << "\n";
        return 1;
    }

    mvlc::listfile::ReadHandle *listfileReadHandle = {};

    try
    {
        listfileReadHandle = zipReader.openEntry(listfileEntryName);
    }
    catch (const std::exception &e)
    {
        std::cerr << fmt::format("Error: could not open listfile entry {} for reading: {}\n", listfileEntryName, e.what());
        return 1;
    }

    auto readerHelper = mvlc::listfile::make_listfile_reader_helper(listfileReadHandle);

    struct VmeConfigs
    {
        std::unique_ptr<VMEConfig> vmeConfig;
        mvlc::CrateConfig crateConfig;
    };

    // VMEConfig and CrateConfig by crateId
    std::unordered_map<u8, VmeConfigs> vmeConfigs;

    for (auto &sysEvent: readerHelper.preamble.systemEvents)
    {
        if (sysEvent.type == system_event::subtype::MVMEConfig)
        {
            auto [vmeConfig, ec] = vme_config::read_vme_config_from_data(sysEvent.contents);

            if (ec)
            {
                std::cerr << fmt::format("Error reading VME config from listfile: {}\n", ec.message());
                return 1;
            }

            auto crateConfig = vmeconfig_to_crateconfig(vmeConfig.get());

            if (vmeConfigs.find(crateConfig.crateId) != vmeConfigs.end())
            {
                std::cerr << fmt::format("Error: duplicate crateId {} in listfile\n", crateConfig.crateId);
                return 1;
            }

            vmeConfigs.emplace(crateConfig.crateId, VmeConfigs{std::move(vmeConfig), std::move(crateConfig)});
        }
    }

    fmt::print("Read {} vme configs from {}\n", vmeConfigs.size(), listfileFilename);

#if 0
    struct MultiCrateProcessing
    {
        // TODO: add splitters and event builders
        std::unordered_map<u8, std::shared_ptr<ReadoutParserContext>> parserContexts;
        std::unordered_map<u8, std::shared_ptr<AnalysisProcessingContext>> analysisContexts;
        std::unordered_map<u8, std::vector<CratePipelineStep>> cratePipelines;
    };


    // Replay crate streams from a single listfile.
    struct ReplayApp
    {
        // TODO: ZipReader, filename
        std::shared_ptr<ReplayJobContext> replayContext;
        MultiCrateProcessing proc;
    };

    // Replay crate streams from multiple listfiles. Will likely need crateId mapping.
    struct ReplayAppMultifile
    {
        // TODO: ZipReaders, filenames
        std::vector<std::shared_ptr<ReplayJobContext>> replayContexts;
        MultiCrateProcessing proc;
        // TODO: or std::vector<MultiCrateProcessing> procs;
    };


    struct ReplayApp
    {
        std::shared_ptr<ReplayJobContext> replayContext;
        std::unordered_map<u8, CratePipeline> cratePipelines;
        std::unordered_map<u8, std::shared_ptr<AnalysisProcessingContext>> analysisContexts;
    };
#endif

    auto replayContext = std::make_shared<ReplayJobContext>();
    replayContext->setName("replay_loop");
    replayContext->lfh = listfileReadHandle;

    std::unordered_map<u8, std::vector<CratePipelineStep>> cratePipelineSteps;
    std::unordered_map<u8, std::shared_ptr<ReadoutParserContext>> parserContexts;
    std::unordered_map<u8, std::shared_ptr<AnalysisProcessingContext>> analysisContexts;

    // producer steps
    for (const auto &[crateId, _]: vmeConfigs)
    {
        auto tmpl = "inproc://crate{0}_stage0_step0_raw_data";
        auto url = fmt::format(tmpl, crateId);
        auto [outputLink, res] = nng::make_pair_link(url);
        if (res)
        {
            spdlog::error("Error creating outputlink {} for {}: {}", url, replayContext->name(), nng_strerror(res));
            return 1;
        }
        auto step = make_replay_step(replayContext, crateId, outputLink);
        step.context->setName(fmt::format("replay_loop_crate{}", crateId));
        cratePipelineSteps[crateId].emplace_back(std::move(step));
    }

    // readout parsers
    for (const auto &[crateId, configs]: vmeConfigs)
    {
        auto ctx = std::shared_ptr<ReadoutParserContext>(make_readout_parser_context(configs.crateConfig));
        ctx->setName(fmt::format("readout_parser_crate{}", crateId));
        auto tmpl = "inproc://crate{0}_stage0_step1_parsed_data";
        auto url = fmt::format(tmpl, ctx->crateId);
        auto [outputLink, res] = nng::make_pair_link(url);
        if (res)
        {
            spdlog::error("Error creating outputlink {} for {}: {}", url, ctx->name(), nng_strerror(res));
            return 1;
        }
        auto step = make_readout_parser_step(ctx, cratePipelineSteps[crateId].back().outputLink, outputLink);
        cratePipelineSteps[crateId].emplace_back(std::move(step));
        parserContexts.emplace(crateId, ctx);
    }

    if (!analysisFilename.empty())
    {
        // analysis consumers
        for (const auto &[crateId, configs]: vmeConfigs)
        {
            auto ctx = std::shared_ptr<AnalysisProcessingContext>(make_analysis_context(analysisFilename, configs.vmeConfig.get()));
            assert(ctx);
            if (ctx && ctx->analysis)
            {
                ctx->setName(fmt::format("analysis_crate{}", crateId));
                ctx->crateId = configs.crateConfig.crateId;
                ctx->runInfo.runId = QFileInfo(listfileFilename.c_str()).baseName();
                ctx->runInfo.isReplay = true;
                ctx->runInfo.infoDict["replaySourceFile"] = listfileFilename.c_str();
                // TODO analysis object id handling / rewriting before beginRun()
                ctx->analysis->beginRun(ctx->runInfo, ctx->asp->vmeConfig_);
                auto step = make_analysis_step(ctx, cratePipelineSteps[crateId].back().outputLink);
                cratePipelineSteps[crateId].emplace_back(std::move(step));
                analysisContexts.emplace(crateId, ctx);
            }
        }
    }
    else
    {
        // test consumers
        for (const auto &[crateId, _]: vmeConfigs)
        {
            auto ctx = std::make_shared<TestConsumerContext>();
            ctx->setName(fmt::format("test_consumer_crate{}", crateId));
            auto step = make_test_consumer_step(ctx, cratePipelineSteps[crateId].back().outputLink);
            cratePipelineSteps[crateId].emplace_back(std::move(step));
        }
    }

    for (auto &[crateId, ctx]: analysisContexts)
    {
        auto asp = ctx->asp.get();
        auto widget = new analysis::ui::AnalysisWidget(asp);
        widget->setAttribute(Qt::WA_DeleteOnClose, true);
        widget->setWindowTitle(fmt::format("Analysis (crate {})", ctx->crateId).c_str());
        widget->show();
        add_widget_close_action(widget);
    }

    CratePipelineMonitorWidget monitorWidget;
    add_widget_close_action(&monitorWidget);

    for (auto &[crateId, steps]: cratePipelineSteps)
    {
        monitorWidget.addPipeline(fmt::format("crate{}", crateId), steps);
    }

    monitorWidget.resize(1000, 400);
    monitorWidget.show();


    QWidget controlsWidget;
    add_widget_close_action(&controlsWidget);
    auto pbStart = new QPushButton("Start");
    auto pbStop = new QPushButton("Stop");
    auto cbAutoRestart = new QCheckBox("Auto-restart");
    auto cbKeepHistoContents = new QCheckBox("Keep histogram contents");
    pbStart->setEnabled(true);
    pbStop->setEnabled(false);
    cbAutoRestart->setChecked(false);
    cbKeepHistoContents->setChecked(false);
    {
        auto l = make_vbox(&controlsWidget);
        l->addWidget(pbStart);
        l->addWidget(pbStop);
        l->addWidget(cbAutoRestart);
        l->addWidget(cbKeepHistoContents);
    }

    auto start_replay = [&]
    {
        for (auto &[crateId, steps]: cratePipelineSteps)
        {
            const bool anyRunning = std::any_of(std::begin(steps), std::end(steps), [] (const auto &step) { return step.context->jobRuntime().isRunning(); });
            if (anyRunning) return;
        }

        if (replayContext->lfh)
        {
            replayContext->lfh->seek(0);
            (void) listfile::read_magic(*replayContext->lfh);
        }

        for (auto &[crateId, ctx]: analysisContexts)
        {
            ctx->runInfo.keepAnalysisState = cbKeepHistoContents->isChecked();
            ctx->analysis->beginRun(ctx->runInfo, ctx->asp->vmeConfig_);

        }

        #ifndef NDEBUG
        for (auto &[crateId, steps]: cratePipelineSteps)
        {
            for (auto &step: steps)
            {
                assert(!step.context->jobRuntime().isRunning());
                assert(!step.context->jobRuntime().isReady());
            }
        }
        #endif

        pbStart->setEnabled(false);
        pbStop->setEnabled(!pbStart->isEnabled());

        // start pipelines
        for (auto &[crateId, steps]: cratePipelineSteps)
        {
            for (auto &step: steps)
            {
                if (!step.context->jobRuntime().isRunning() && !step.context->jobRuntime().isReady())
                {
                    if (start_job(*step.context))
                        spdlog::info("started job {}", step.context->name());
                    else
                    {
                        spdlog::error("error starting job {}", step.context->name());
                        return;
                    }
                }
                else
                {
                    spdlog::info("job {} was already running", step.context->name());
                }
            }
        }
    };

    bool manuallyStopped = false;

    auto stop_replay = [&] (bool immediateShutdown = false)
    {
        if (immediateShutdown)
            spdlog::warn("begin immediate shutdown");
        else
            spdlog::warn("begin graceful shutdown");

        StopWatch sw;

        // Tell the shared replay context to quit, basically terminating the
        // first step of each pipeline.
        replayContext->quit();

        for (auto &[crateId, steps]: cratePipelineSteps)
        {
            spdlog::info("terminating pipeline for crate{}", crateId);
            if (immediateShutdown)
                quit_pipeline(steps);
            else
                shutdown_pipeline(steps);
        }

        if (immediateShutdown)
        {
            StopWatch swEmpty;
            for (auto &[crateId, steps]: cratePipelineSteps)
            {
                auto msgCount = empty_pipeline_inputs(steps);
                spdlog::info("emptied pipeline for crate{}: {} messages", crateId, msgCount);
            }
            spdlog::warn("empty pipelines took {} ms", swEmpty.interval().count() / 1000.0);
        }

        auto dt = sw.interval();

        if (immediateShutdown)
            spdlog::warn("end immediate shutdown (dt={} ms)", dt.count() / 1000.0);
        else
            spdlog::warn("end graceful shutdown (dt={} ms)", dt.count() / 1000.0);
    };

    auto log_counters = [&]
    {
        for (const auto &[crateId, steps]: cratePipelineSteps)
        {
            for (const auto &step: steps)
            {
                if (step.reader)
                    log_socket_work_counters(step.context->readerCounters().copy(), fmt::format("step={}, reader", step.context->name()));
                if (step.writer)
                    log_socket_work_counters(step.context->writerCounters().copy(), fmt::format("step={}, writer", step.context->name()));
            }
        }
    };

    auto on_job_finished = [&] (const std::shared_ptr<JobContextInterface> &ctx_)
    {
        if (replayContext->jobRuntime().isReady() && !replayContext->lastResult().has_value())
        {
            spdlog::info("replay finished, starting shutdown");
            stop_replay();
            log_counters();

            if (!analysisContexts.empty())
            {
                std::map<u8, mesytec::mvlc::readout_parser::ReadoutParserCounters> parserCounters;
                for (const auto &[crateId, ctx]: parserContexts)
                    parserCounters[crateId] = ctx->parserCounters.copy();
                std::map<u8, std::shared_ptr<analysis::Analysis>> analyses;
                for (const auto &[crateId, ctx]: analysisContexts)
                    analyses[crateId] = ctx->analysis;
                auto &[crateId, ctx] = *analysisContexts.begin();
                analysis::save_run_statistics_to_json(ctx->runInfo, ctx->runInfo.runId + ".json", parserCounters, analyses);
            }

            if (!manuallyStopped && cbAutoRestart->isChecked())
            {
                start_replay();
            }
            pbStart->setEnabled(true);
        }
    };

    QObject::connect(pbStart, &QPushButton::clicked, &controlsWidget, [&] { manuallyStopped = false; start_replay(); });
    QObject::connect(pbStop, &QPushButton::clicked, &controlsWidget, [&] { manuallyStopped = true; stop_replay(true); });

    controlsWidget.show();


    QTimer timer;

    timer.setInterval(16);
    timer.start();

    QObject::connect(&timer, &QTimer::timeout, [&]
    {
        if (signal_received())
        {
            spdlog::warn("signal received, shutting down");
            app.quit();
        }
    });

    auto aQuit = new QAction("&Quit"); // FIXME: leaking this global action at the moment
    aQuit->setShortcut(QSL("Ctrl+Q"));
    aQuit->setShortcutContext(Qt::ApplicationShortcut);
    QObject::connect(aQuit, &QAction::triggered, &app, &QApplication::quit);
    JobPoller jobPoller;

    QObject::connect(&jobPoller, &JobPoller::jobReady, on_job_finished);

    QObject::connect(&jobPoller, &JobPoller::jobReady, [] (const std::shared_ptr<JobContextInterface> &ctx)
    {
        spdlog::warn("jobPoller: job {} ready\n", ctx->name());
    });


    for (auto &[crateId, steps]: cratePipelineSteps)
    {
        for (auto &step: steps)
        {
            jobPoller.addJob(step.context);
        }
    }

    auto jobPollerFuture = std::async(std::launch::async, [&] { jobPoller.loop(); });



    #if 0
    for (int i=0; i<3; ++i)
    {
        auto w = new QMainWindow;
        w->setAttribute(Qt::WA_DeleteOnClose, true);
        w->setWindowTitle(QString("mainwindow %1").arg(i));
        w->show();
        auto tb = w->addToolBar("toolbar");
        auto mb = w->menuBar();
        auto mFile = mb->addMenu("&File");
        mFile->addAction(aQuit);
    }
    #endif

    int ret = app.exec();

    stop_replay();
    jobPoller.quit();
    jobPollerFuture.wait();

    spdlog::info("crate pipelines terminated");

    for (auto &[crateId, steps]: cratePipelineSteps)
    {
        spdlog::info("closing pipeline for crate{}", crateId);
        if (int res = close_pipeline(steps))
        {
            spdlog::warn("{}", nng_strerror(res));
        }
    }

    spdlog::info("crate pipelines closed");

    return ret;
}
