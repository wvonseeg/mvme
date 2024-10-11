#ifndef B775D3EE_03C4_46EA_96F8_DF6723990728
#define B775D3EE_03C4_46EA_96F8_DF6723990728

#include "mdpp_sampling.h"

#include "mvme_qwt.h"

#include <QDebug>

namespace mesytec::mvme
{

using TraceBuffer = QList<ChannelTrace>;
using ModuleTraceHistory = std::vector<TraceBuffer>; // indexed by the traces channel number
using TraceHistoryMap = QMap<QUuid, ModuleTraceHistory>;


// InterpolatedSamples takes precendence. if empty raw samples are plotted.
struct MdppChannelTracePlotData: public QwtSeriesData<QPointF>
{
    const ChannelTrace *trace_ = nullptr;
    mutable QRectF boundingRectCache_;

    // Set the event data to plot
    void setTrace(const ChannelTrace *trace)
    {
         trace_ = trace;
         boundingRectCache_ = {};
    }

    const ChannelTrace *getTrace() const { return trace_; }

    QRectF boundingRect() const override
    {
        if (!trace_ || (trace_->samples.empty() && trace_->interpolated.empty()))
            return {};

        if (!boundingRectCache_.isValid())
            boundingRectCache_ = calculateBoundingRect();

        return boundingRectCache_;
    }

    QRectF calculateBoundingRect() const
    {
        // Determine min and max y values for the trace.

        if (trace_->interpolated.empty())
        {
            auto &samples = trace_->samples;
            auto minMax = std::minmax_element(std::begin(samples), std::end(samples));

            if (minMax.first != std::end(samples) && minMax.second != std::end(samples))
            {
                QPointF topLeft(0, *minMax.second);
                QPointF bottomRight(samples.size(), *minMax.first);
                return QRectF(topLeft, bottomRight);
            }
        }
        else
        {
            auto &samples = trace_->interpolated;
            auto minMax = std::minmax_element(std::begin(samples), std::end(samples),
                            [](const auto &a, const auto &b) { return a.second < b.second; });

            if (minMax.first != std::end(samples) && minMax.second != std::end(samples))
            {
                double minY = minMax.first->second;
                double maxY = minMax.second->second;
                double maxX = samples.back().first; // last sample must contain the largest x coordinate

                QPointF topLeft(0, maxY);
                QPointF bottomRight(maxX, minY);
                return QRectF(topLeft, bottomRight);
            }
        }

        return {};
    }

    size_t size() const override
    {
        if (trace_)
        {
            if (trace_->interpolated.empty())
                return trace_->samples.size();
            return trace_->interpolated.size();
        }
        return 0;
    }

    QPointF sample(size_t i) const override
    {
        if (trace_)
        {
            if (trace_->interpolated.empty() && i < static_cast<size_t>(trace_->samples.size()))
                return QPointF(i, trace_->samples[i]);
            else if (i < static_cast<size_t>(trace_->interpolated.size()))
                return QPointF(trace_->interpolated[i].first, trace_->interpolated[i].second);
        }

        return {};
    }
};

}

#endif /* B775D3EE_03C4_46EA_96F8_DF6723990728 */
