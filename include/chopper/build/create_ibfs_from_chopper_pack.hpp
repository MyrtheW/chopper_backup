#pragma once

#include <fstream>
#include <unordered_set>
#include <seqan3/std/ranges>

#include <seqan3/alphabet/nucleotide/dna4.hpp>
#include <seqan3/core/debug_stream.hpp>
#include <seqan3/io/sequence_file/all.hpp>
#include <seqan3/range/views/kmer_hash.hpp>
#include <seqan3/search/dream_index/interleaved_bloom_filter.hpp>

#include <chopper/build/build_config.hpp>
#include <chopper/build/compute_bin_size.hpp>
#include <chopper/build/read_chopper_pack_file.hpp>

struct file_traits : public seqan3::sequence_file_input_default_traits_dna
{
    using sequence_alphabet = seqan3::dna4;
};

using sequence_file_t = seqan3::sequence_file_input<file_traits,
                                                    seqan3::fields<seqan3::field::seq>,
                                                    seqan3::type_list<seqan3::format_fasta, seqan3::format_fastq>>;

std::unordered_set<size_t> compute_kmers(build_config const & config, chopper_pack_record const & record)
{
    std::unordered_set<size_t> kmers{};

    for (auto const & filename : record.filenames)
        for (auto && [seq] : sequence_file_t{filename})
            for (auto hash : seq | seqan3::views::kmer_hash(seqan3::ungapped{config.k}))
                kmers.insert(hash);

    return kmers;
}

void compute_kmers(std::unordered_set<size_t> & kmers,
                   build_config const & config,
                   chopper_pack_record const & record)
{
    for (auto const & filename : record.filenames)
        for (auto && [seq] : sequence_file_t{filename})
            for (auto hash : seq | seqan3::views::kmer_hash(seqan3::ungapped{config.k}))
                kmers.insert(hash);
}

// automatically does naive splitting if number_of_bins > 1
void insert_into_ibf(std::unordered_set<size_t> const & kmers,
                     size_t const number_of_bins,
                     size_t const bin_index,
                     seqan3::interleaved_bloom_filter<> & ibf)
{
    size_t const kmers_per_chunk = (kmers.size() / number_of_bins) + 1;
    auto it = kmers.begin();
    for (size_t chunk = 0; chunk < number_of_bins; ++chunk)
        for (size_t i = chunk * kmers_per_chunk; i < std::min((chunk + 1) * kmers_per_chunk, kmers.size()); ++i, ++it)
            ibf.emplace(*it, seqan3::bin_index{bin_index + chunk});
}

void insert_into_ibf(build_config const & config,
                     chopper_pack_record const & record,
                     seqan3::interleaved_bloom_filter<> & hibf)
{
    for (auto const & filename : record.filenames)
    {
        for (auto && [seq] : sequence_file_t{filename})
        {
            for (auto hash : seq | seqan3::views::kmer_hash(seqan3::ungapped{config.k}))
            {
                assert(record.bin_indices.back() >= 0);
                hibf.emplace(hash, seqan3::bin_index{static_cast<size_t>(record.bin_indices.back())});
            }
        }
    }
}

void build(std::unordered_set<size_t> & parent_kmers,
           lemon::ListDigraph::Node const & current_node,
           build_data & data,
           build_config const & config)
{
    std::unordered_set<size_t> max_bin_kmers{};

    auto & current_node_data = data.node_map[current_node];

    std::vector<int64_t> ibf_positions(current_node_data.number_of_technical_bins, -1);

    size_t number_of_max_bin_technical_bins{};
    if (current_node_data.favourite_child != lemon::INVALID) // max bin is a merged bin
    {
        build(max_bin_kmers, current_node_data.favourite_child, data, config); // recursively initialize favourite child first
        ibf_positions[current_node_data.max_bin_index] = data.ibfs.size() - 1;
        number_of_max_bin_technical_bins = 1;
    }
    else // there a max bin, that is ot a merged bin
    {
        // we assume that the max record is at the beginning of the list of remaining records.
        auto const & record = current_node_data.remaining_records[0];
        compute_kmers(max_bin_kmers, config, record);
        number_of_max_bin_technical_bins = record.number_of_bins.back();
    }

    // construct High Level IBF
    seqan3::bin_size const bin_size{compute_bin_size(config, max_bin_kmers.size() / number_of_max_bin_technical_bins)};
    seqan3::bin_count const bin_count{current_node_data.number_of_technical_bins};
    seqan3::interleaved_bloom_filter ibf{bin_count, bin_size, seqan3::hash_function_count{config.hash_funs}};

    if (config.verbose)
        seqan3::debug_stream << "  > Initialised LIBF with bin size " << bin_size.get() << std::endl;

    insert_into_ibf(max_bin_kmers, number_of_max_bin_technical_bins, current_node_data.max_bin_index, ibf);

    parent_kmers.merge(max_bin_kmers);
    max_bin_kmers.clear(); // reduce memory peak

    // parse all other children (merged bins) of the current ibf
    for (lemon::ListDigraph::OutArcIt arc_it(data.ibf_graph, current_node); arc_it != lemon::INVALID; ++arc_it)
    {
        auto child = data.ibf_graph.target(arc_it);
        auto & child_data = data.node_map[child];
        if (child != current_node_data.favourite_child)
        {
            std::unordered_set<size_t> kmers{}; // todo: maybe it is more efficient if this is declared outside and cleared every iteration
            build(kmers, child, data, config); // also appends that childs counts to 'kmers'
            insert_into_ibf(kmers, 1, child_data.parent_bin_index, ibf);
            parent_kmers.merge(kmers);
            ibf_positions[child_data.parent_bin_index] = data.ibfs.size() - 1;
        }
    }

    for (auto const & record : current_node_data.remaining_records)
    {
        std::unordered_set<size_t> kmers{};
        compute_kmers(kmers, config, record);
        insert_into_ibf(kmers, record.number_of_bins.back(), record.bin_indices.back(), ibf);
        parent_kmers.merge(kmers);
    }

    data.ibfs.push_back(std::move(ibf));

    for (auto & pos : ibf_positions)
        if (pos == -1)
            pos = data.ibfs.size() - 1;

    data.ibf_mapping.push_back(std::move(ibf_positions));
}

void create_ibfs_from_chopper_pack(build_data & data, build_config const & config)
{
    read_chopper_pack_file(data, config.chopper_pack_filename);

    lemon::ListDigraph::Node root = data.ibf_graph.nodeFromId(0); // root node = high level IBF node
    auto & root_node_data = data.node_map[root];

    std::vector<int64_t> ibf_positions(root_node_data.number_of_technical_bins, 0);

    std::unordered_set<size_t> max_bin_kmers{};

    size_t number_of_max_bin_technical_bins{};
    if (root_node_data.favourite_child != lemon::INVALID) // max bin is a merged bin
    {
        build(max_bin_kmers, root_node_data.favourite_child, data, config); // recursively initialize favourite child first
        ibf_positions[root_node_data.max_bin_index] = data.ibfs.size() - 1;
        number_of_max_bin_technical_bins = 1;
    }
    else // there a max bin, that is ot a merged bin
    {
        // we assume that the max record is at the beginning of the list of remaining records.
        auto const & record = root_node_data.remaining_records[0];
        compute_kmers(max_bin_kmers, config, record);
        assert(record.number_of_bins.size() == 1);
        number_of_max_bin_technical_bins = record.number_of_bins.front();
    }

    // construct High Level IBF
    seqan3::bin_size const bin_size{compute_bin_size(config, max_bin_kmers.size() / number_of_max_bin_technical_bins)};
    seqan3::bin_count const bin_count{root_node_data.number_of_technical_bins};
    seqan3::interleaved_bloom_filter high_level_ibf{bin_count, bin_size, seqan3::hash_function_count{config.hash_funs}};

    if (config.verbose)
        seqan3::debug_stream << "  > Initialised High Level IBF with bin size " << bin_size.get() << std::endl;

    insert_into_ibf(max_bin_kmers, number_of_max_bin_technical_bins, root_node_data.max_bin_index, high_level_ibf);

    max_bin_kmers.clear(); // clear memory allocation

    // parse all other children (merged bins) of the high level-ibf (will build the whole HIBF)
    for (lemon::ListDigraph::OutArcIt arc_it(data.ibf_graph, root); arc_it != lemon::INVALID; ++arc_it)
    {
        auto child = data.ibf_graph.target(arc_it);
        auto & child_data = data.node_map[child];
        if (child != root_node_data.favourite_child)
        {
            std::unordered_set<size_t> kmers{};
            build(kmers, child, data, config); // also appends that childs counts to 'kmers'
            insert_into_ibf(kmers, 1, child_data.parent_bin_index, high_level_ibf);
            ibf_positions[child_data.parent_bin_index] = data.ibfs.size() - 1;
        }
    }

    for (auto const & record : root_node_data.remaining_records)
        insert_into_ibf(config, record, high_level_ibf);

    data.ibfs.insert(data.ibfs.begin(), std::move(high_level_ibf)); // insert High level at the beginning
    data.ibf_mapping.insert(data.ibf_mapping.begin(), std::move(ibf_positions));
}