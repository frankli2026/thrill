/*******************************************************************************
 * benchmarks/prefix_doubling/prefix_doubling.cpp
 *
 * Part of Project Thrill - http://project-thrill.org
 *
 * Copyright (C) 2016 Florian Kurpicz <florian.kurpicz@tu-dortmund.de>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#include <thrill/api/allgather.hpp>
#include <thrill/api/cache.hpp>
#include <thrill/api/dia.hpp>
#include <thrill/api/distribute.hpp>
#include <thrill/api/distribute_from.hpp>
#include <thrill/api/gather.hpp>
#include <thrill/api/generate.hpp>
#include <thrill/api/groupby.hpp>
#include <thrill/api/merge.hpp>
#include <thrill/api/prefixsum.hpp>
#include <thrill/api/read_binary.hpp>
#include <thrill/api/size.hpp>
#include <thrill/api/sort.hpp>
#include <thrill/api/sum.hpp>
#include <thrill/api/window.hpp>
#include <thrill/api/write_binary.hpp>
#include <thrill/api/zip.hpp>
#include <thrill/common/cmdline_parser.hpp>
#include <thrill/common/logger.hpp>
#include <thrill/core/multiway_merge.hpp>

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

bool debug_print = false;
bool debug = true;

using namespace thrill; // NOLINT
using thrill::common::RingBuffer;

//! A pair (index, t=T[index]).
template <typename AlphabetType>  
struct IndexOneMer {
    size_t       index;
    AlphabetType t;

    friend std::ostream& operator << (std::ostream& os, const IndexOneMer& iom) {
        return os << '[' << iom.index << ',' << iom.t << ']';
    }
} THRILL_ATTRIBUTE_PACKED;

//! A pair (rank, index)
struct RankIndex {
    size_t rank;
    size_t index;

    friend std::ostream& operator << (std::ostream& os, const RankIndex& ri) {
        return os << '(' << ri.index << '|' << ri.rank << ')';
    }
} THRILL_ATTRIBUTE_PACKED;

//! A triple (rank_1, rank_2, index)
struct RankRankIndex {
    size_t rank1;
    size_t rank2;
    size_t index;

    friend std::ostream& operator << (std::ostream& os, const RankRankIndex& rri) {
        return os << "( i: " << rri.index << "| r1: " << rri.rank1 << "| r2: " << rri.rank2 << ")";
    }
} THRILL_ATTRIBUTE_PACKED;

template <typename InputDIA>
DIA<size_t> PrefixDoubling(Context& ctx, const InputDIA& input_dia, size_t input_size) {
    
    using Char = typename InputDIA::ValueType;
    using IndexOneMer = ::IndexOneMer<Char>;

    auto all_indices = Generate(
        ctx,
        [](size_t index) { return index; },
        input_size).Cache();

    auto one_mers_sorted = 
        input_dia
        .template FlatWindow<IndexOneMer>(
            2,
            [input_size](size_t index, const RingBuffer<Char>& rb, auto emit) {
                emit(IndexOneMer {index, rb[0]});
                if(index == input_size - 2) emit(IndexOneMer {index + 1, rb[1]});
        })
        .Sort([](const IndexOneMer& a, const IndexOneMer& b) {
            return a.t < b.t;
        }).Keep();

    if(debug_print)
        one_mers_sorted.Print("one_mers_sorted");

    auto sa =
        one_mers_sorted
        .Map([](const IndexOneMer& iom) ->size_t { return iom.index; });

    if (debug_print)
        sa.Print("sa");

    DIA<size_t> rebucket =
        one_mers_sorted
        .template FlatWindow<size_t>(
            2,
            [input_size](size_t index, const RingBuffer<IndexOneMer>& rb, auto emit) {
                if (index == 0) emit(0);
                if (rb[0].t == rb[1].t) emit(0);
                else emit(index + 1);
                if (index == input_size - 2) {
                    if (rb[0].t == rb[1].t) emit(0);
                    else emit(index + 2);
                }
        })
        .template FlatWindow<size_t>(
            2,
            [input_size](size_t index, const RingBuffer<size_t>& rb, auto emit) {
                if(index == 0) emit(0);
                if(rb[0] != 0) emit(rb[0]);
                if(index == input_size - 2 and rb[1] != 0) emit(rb[1]);
        });

    if (debug_print)
        rebucket.Print("rebucket");

    size_t rebucket_size =
        rebucket.Size();

    DIA<size_t> rebucket_final =
        rebucket
        .template FlatWindow<size_t>(
            2,
            [rebucket_size, input_size](size_t index, const RingBuffer<size_t>& rb, auto emit) {
                assert(rb[1] > rb[0]);
                for(size_t i = 0; i < rb[1] - rb[0]; ++i)
                    emit(rb[0]);
                if (index == rebucket_size - 2) {
                    for(size_t i = 0; i < input_size - rb[1]; ++i)
                        emit(rb[1]);
                }
        });

    if (debug_print)
        rebucket_final.Print("rebucket_final");

    uint8_t shifted_exp = 1;

    auto isa =
        sa
        .Zip(
            rebucket_final,
            [](size_t sa, size_t rb) {
                return RankIndex {sa, rb};
        })
        .Sort([](const RankIndex& a, const RankIndex& b) {
            return a.rank < b.rank;   
        })
        .Map(
            [](const RankIndex& ri) { 
                return ri.index;
        });

    if (debug_print)
        isa.Print("isa");

    size_t shift_by = 1 << shifted_exp++;

    auto shifted_isa =
        isa
        .template FlatWindow<size_t>(
            shift_by,
            [input_size, shift_by](size_t index, const RingBuffer<size_t>& rb, auto emit) {
                emit(rb[shift_by - 1]);
                if(index == input_size - shift_by)
                    for(size_t i = index + 1; i < input_size; ++i)
                        emit(0);
            }
        );
    
    if (debug_print)
        shifted_isa.Print("shifted_isa");

    DIA<RankRankIndex> triple_sorted =
        Zip(
            [](size_t a, size_t b, size_t c) {
                return RankRankIndex {a, b, c};
            },
            isa, shifted_isa, all_indices
        )
        .Sort([](const RankRankIndex& a, const RankRankIndex& b) {
            if (a.rank1 == b.rank1) {
                if (a.rank2 == b.rank2) return a.index < b.index;
                else return a.rank2 < b.rank2;
            } else return a.rank1 < b.rank1;
        }).Keep();

    if (debug_print)
        triple_sorted.Print("triple_sorted_outside_loop");

    while(true) {
        LOG << "Shift the SA by " << shift_by << " positions";
        rebucket =
            triple_sorted
            .template FlatWindow<size_t>(
                2,
                [input_size](size_t index, const RingBuffer<RankRankIndex>& rb, auto emit) {
                    if (index == 0) emit(0);
                    if (rb[0].rank1 == rb[1].rank1 and rb[0].rank2 == rb[1].rank2) emit(0);
                    else emit(index + 1);
            })
            .template FlatWindow<size_t>(
                2,
                [input_size](size_t index, const RingBuffer<size_t>& rb, auto emit) {
                    if(index == 0) emit(0);
                    if(rb[0] != 0) emit(rb[0]);
                    if(index == input_size - 2 and rb[1] != 0) emit(rb[1]);
            });

        if (debug_print)
            rebucket.Print("rebucket");

        rebucket_size =
            rebucket.Size();

        // If each suffix is unique regarding their 2h-prefix, we have computed
        // the suffix array and can return it. 
        if (rebucket_size == input_size) {
            auto suffix_array =
                triple_sorted
                .Map([](const RankRankIndex& rri) { return rri.index;
            });

            if (debug_print)
                suffix_array.Print("suffix_array");

            return suffix_array.Collapse();
        }

        rebucket_final =
            rebucket
            .template FlatWindow<size_t>(
                2,
                [rebucket_size, input_size](size_t index, const RingBuffer<size_t>& rb, auto emit) {
                    assert(rb[1] > rb[0]);
                    for(size_t i = 0; i < rb[1] - rb[0]; ++i)
                        emit(rb[0]);
                    if (index == rebucket_size - 2) {
                        for(size_t i = 0; i < input_size - rb[1]; ++i)
                            emit(rb[1]);
                    }
            });

        if (debug_print)
            rebucket_final.Print("rebucket_final");

        auto sa_loop =
            triple_sorted
            .Map(
                [](const RankRankIndex& rri) {
                    return rri.index;
            });

        if (debug_print)
            sa_loop.Print("sa_loop");

        auto isa_loop =
            sa_loop
            .Zip(
                rebucket_final,
                [](size_t sa, size_t rb) {
                    return RankIndex {sa, rb};
            })
            .Sort([](const RankIndex& a, const RankIndex& b) {
                return a.rank < b.rank;   
            })
            .Map(
                [](const RankIndex& ri) { 
                    return ri.index;
            });

        if (debug_print)
            isa_loop.Print("isa_loop");

        shift_by = 1 << shifted_exp++;

        auto shifted_isa_loop =
            isa_loop
            .template FlatWindow<size_t>(
                shift_by,
                [input_size, shift_by](size_t index, const RingBuffer<size_t>& rb, auto emit) {
                    emit(rb[shift_by - 1]);
                    if(index == input_size - shift_by)
                        for(size_t i = index + 1; i < input_size; ++i)
                            emit(0);
                }
            );

        if (debug_print)
            shifted_isa_loop.Print("shifted_isa_loop");

        triple_sorted =
            Zip(
                [](size_t a, size_t b, size_t c) {
                    return RankRankIndex {a, b, c};
                },
                isa_loop, shifted_isa_loop, all_indices
            )
            .Sort([](const RankRankIndex& a, const RankRankIndex& b) {
                if (a.rank1 == b.rank1) {
                    if (a.rank2 == b.rank2) return a.index < b.index;
                    else return a.rank2 < b.rank2;
                } else return a.rank1 < b.rank1;
            }).Keep();

        if (debug_print)
            triple_sorted.Print("triple_sorted");
    }

}

/*!
 * Class to encapsulate all
 */
class StartPrefixDoubling
{
public:
    StartPrefixDoubling(
        Context& ctx,
        const std::string& input_path, const std::string& output_path,
        bool text_output_flag,
        bool check_flag,
        bool input_verbatim)
        : ctx_(ctx),
          input_path_(input_path), output_path_(output_path),
          text_output_flag_(text_output_flag),
          check_flag_(check_flag),
          input_verbatim_(input_verbatim) { }

    void Run() {
        if (input_verbatim_) {
            // take path as verbatim text
            std::vector<uint8_t> input_vec(input_path_.begin(), input_path_.end());
            auto input_dia = Distribute<uint8_t>(ctx_, input_vec);
            if (debug_print) input_dia.Print("input");

            StartPrefixDoublingInput(input_dia, input_vec.size());
        } 
        else {
            auto input_dia = ReadBinary<uint8_t>(ctx_, input_path_);
            size_t input_size = input_dia.Size();
            StartPrefixDoublingInput(input_dia, input_size);
        }
    }

    template <typename InputDIA>
    void StartPrefixDoublingInput(const InputDIA& input_dia, uint64_t input_size) {

        auto suffix_array = PrefixDoubling(ctx_, input_dia, input_size);
        if (output_path_.size()) {
            suffix_array.WriteBinary(output_path_);
        }

        if (check_flag_) {
            LOG1 << "checking suffix array...";

            //if (!CheckSA(input_dia, suffix_array)) {
            //    throw std::runtime_error("Suffix array is invalid!");
            //}
            //else {
            //    LOG1 << "okay.";
            //}
        }
    }

protected:
    Context& ctx_;

    std::string input_path_;
    std::string output_path_;

    bool text_output_flag_;
    bool check_flag_;
    bool input_verbatim_;
    
};

int main(int argc, char* argv[]) {
    common::CmdlineParser cp;

    cp.SetDescription("A prefix doubling suffix array construction algorithm.");
    cp.SetAuthor("Florian Kurpicz <florian.kurpicz@tu-dortmund.de>");

    std::string input_path, output_path;
    bool text_output_flag = false;
    bool check_flag = false;
    bool input_verbatim = false;

    cp.AddParamString("input", input_path,
                      "Path to input file (or verbatim text).\n"
                      "  The special inputs 'random' and 'unary' generate "
                      "such text on-the-fly.");
    cp.AddFlag('c', "check", check_flag,
               "Check suffix array for correctness.");
    cp.AddFlag('t', "text", text_output_flag,
               "Print out suffix array in readable text.");
    cp.AddString('o', "output", output_path,
                 "Output suffix array to given path.");
    cp.AddFlag('v', "verbatim", input_verbatim,
               "Consider \"input\" as verbatim text to construct "
               "suffix array on.");
    cp.AddFlag('d', "debug", debug_print,
               "Print debug info.");

    // process command line
    if (!cp.Process(argc, argv))
        return -1;

    return Run(
        [&](Context& ctx) {
            return StartPrefixDoubling(ctx,
                            input_path, output_path,
                            text_output_flag,
                            check_flag,
                            input_verbatim).Run();
        });
}
/******************************************************************************/
