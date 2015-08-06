/*******************************************************************************
 * c7a/api/scatter.hpp
 *
 * Part of Project c7a.
 *
 * Copyright (C) 2015 Timo Bingmann <tb@panthema.net>
 *
 * This file has no license. Only Chuck Norris can compile it.
 ******************************************************************************/

#pragma once
#ifndef C7A_API_SCATTER_HEADER
#define C7A_API_SCATTER_HEADER

#include <c7a/api/dia.hpp>
#include <c7a/api/dop_node.hpp>
#include <c7a/common/logger.hpp>
#include <c7a/common/math.hpp>

#include <string>
#include <vector>

namespace c7a {
namespace api {

//! \addtogroup api Interface
//! \{

template <typename ValueType>
class ScatterNode : public DOpNode<ValueType>
{
public:
    using Super = DOpNode<ValueType>;
    using Super::context_;
    using Super::result_file_;

    ScatterNode(Context& ctx,
                const std::vector<ValueType>& in_vector,
                size_t source_id,
                StatsNode* stats_node)
        : DOpNode<ValueType>(ctx, { }, "Scatter", stats_node),
          in_vector_(in_vector),
          source_id_(source_id),
          channel_(ctx.data_manager().GetNewChannel()),
          emitters_(channel_->OpenWriters())
    { }

    //! Executes the scatter operation: source sends out its data.
    void Execute() override {

        if (context_.my_rank() == source_id_)
        {
            size_t in_size = in_vector_.size();

            for (size_t w = 0; w < emitters_.size(); ++w) {

                size_t local_begin, local_end;
                std::tie(local_begin, local_end) =
                    common::CalculateLocalRange(in_size, emitters_.size(), w);

                for (size_t i = local_begin; i < local_end; ++i) {
                    emitters_[w](in_vector_[i]);
                }
            }
        }

        // close channel inputs.
        for (size_t w = 0; w < emitters_.size(); ++w) {
            emitters_[w].Close();
        }
    }

    void PushData() override {
        data::Channel::CachingConcatReader readers = channel_->OpenCachingReader();

        while (readers.HasNext()) {
            ValueType v = readers.Next<ValueType>();
            for (auto func : DIANode<ValueType>::callbacks_) {
                func(v);
            }
        }
    }

    void Dispose() override { }

    auto ProduceStack() {
        return FunctionStack<ValueType>();
    }

    /*!
     * Returns "[AllGatherNode]" and its id as a string.
     * \return "[AllGatherNode]"
     */
    std::string ToString() override {
        return "[AllGatherNode] Id: " + result_file_.ToString();
    }

private:
    //! Vector pointer to read elements from.
    const std::vector<ValueType>& in_vector_;
    //! source worker id, which sends vector
    size_t source_id_;

    data::ChannelPtr channel_;
    std::vector<data::BlockWriter> emitters_;
};

template <typename ValueType>
auto Scatter(
    Context & ctx,
    const std::vector<ValueType>&in_vector, size_t source_id) {

    using ScatterNode = api::ScatterNode<ValueType>;

    StatsNode* stats_node = ctx.stats_graph().AddNode("Scatter", "DOp");

    auto shared_node =
        std::make_shared<ScatterNode>(
            ctx, in_vector, source_id, stats_node);

    auto scatter_stack = shared_node->ProduceStack();

    return DIARef<ValueType, decltype(scatter_stack)>(
        shared_node, scatter_stack, { stats_node });
}

//! \}

} // namespace api
} // namespace c7a

#endif // !C7A_API_SCATTER_HEADER

/******************************************************************************/