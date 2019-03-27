﻿/**
 * FILE: reader.cpp
 * Created: Nov 26, 2017 Sun
 * @copyright 2016-19 Clarity Genomics BVBA
 * 
 * Processes Reads file, creates Read objects, and pushes them to a queue for further pick−up by Processor
 */

#include <vector>
#include <sstream> // std::stringstream
#include <ios> // std::ios_base
#include <algorithm> // find, find_if
#include <chrono> // std::chrono
#include <iomanip> // std::precision
#include <locale> // std::isspace

#include "reader.hpp"
#include "gzip.hpp"

Reader::Reader(std::string id, std::ifstream &ifs, bool is_gzipped, KeyValueDatabase & kvdb)
	:
	id(id),
	is_gzipped(is_gzipped),
	gzip(is_gzipped),
	kvdb(kvdb),
	ifs(ifs)
{} // ~Reader::Reader

Reader::~Reader()
{
	if (ifs.is_open())
		ifs.close();
}

bool Reader::loadReadByIdx(Runopts & opts, Read & read)
{
	std::stringstream ss;
	bool isok = false;

	std::ifstream ifs(opts.readsfile, std::ios_base::in | std::ios_base::binary);
	if (!ifs.is_open()) 
	{
		std::cerr << STAMP << "failed to open " << opts.readsfile << std::endl;
		exit(EXIT_FAILURE);
	}
	else
	{
		std::string line;
		unsigned int read_id = 0; // read ID
		bool isFastq = true;
		Gzip gzip(opts.is_gz);

		auto t = std::chrono::high_resolution_clock::now();

		// read lines from the reads file
		for (int count = 0, stat = 0; ; ) // count lines in a single read
		{
			stat = gzip.getline(ifs, line);
			if (stat == RL_END) break;

			if (stat == RL_ERR)
			{
				std::cerr << STAMP << "ERROR reading from Reads file. Exiting..." << std::endl;
				exit(1);
			}

			if (line.empty()) continue;

			line.erase(std::find_if(line.rbegin(), line.rend(), [l = std::locale{}](auto ch) { return !std::isspace(ch, l); }).base(), line.end());

			if ( line[0] == FASTA_HEADER_START || line[0] == FASTQ_HEADER_START )
			{
				if (!read.isEmpty) {
					isok = true;
					break; // read is ready
				}

				// add header -->
				if (read_id == read.id)
				{
					isFastq = (line[0] == FASTQ_HEADER_START);
					read.format = isFastq ? Format::FASTQ : Format::FASTA;
					read.header = line;
					read.isEmpty = false;
				}
				else {
					++read_id;
					count = 0; // for fastq
				}
			} // ~if header line
			else if ( !read.isEmpty )
			{
				// add sequence -->
				if ( isFastq )
				{
					++count;
					if ( line[0] == '+' ) continue;
					if ( count == 3 )
					{
						read.quality = line; // last line in Fastq read
						continue;
					}
				}
				read.sequence += line;
			}
		} // ~for getline

		std::chrono::duration<double> elapsed = std::chrono::high_resolution_clock::now() - t;

		//ss << id << " thread: " << std::this_thread::get_id() << " done. Elapsed time: "
		//	<< std::setprecision(2) << std::fixed << elapsed.count() << " sec Reads added: " << read_id << std::endl;
		//std::cout << ss.str(); ss.str("");
	}

	ifs.close();

	return isok;

} // ~Reader::loadRead

bool Reader::loadReadById(Runopts & opts, Read & read)
{
	return true;
} // ~Reader::loadReadById


/** 
 * return next read from the reads file on each call 
 */
Read Reader::nextread(Runopts & opts)
{
	std::string line;
	Read read; // an empty read

	// read lines from the reads file and create Read object
	for (int count = 0, stat = last_stat; ; ++count) // count lines in a single record/read
	{
		stat = gzip.getline(ifs, line);
		++line_count;

		if (stat == RL_END)
		{
			// push the last Read to the queue
			if (!read.isEmpty)
			{
				++read_count;
				read.init(read_count, kvdb, opts); // load alignment statistics from DB
				//readQueue.push(read); // the last read
				is_done = true;
			}
			break;
		}

		if (stat == RL_ERR)
		{
			std::cerr << STAMP << "ERROR reading from file stream. Exiting..." << std::endl;
			exit(1);
		}

		if (line.empty())
		{
			--count;
			--line_count;
			continue;
		}

		// left trim space and '>' or '@'
		//line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](auto ch) {return !(ch == FASTA_HEADER_START || ch == FASTQ_HEADER_START);}));
		// right-trim whitespace in place (removes '\r' too)
		line.erase(std::find_if(line.rbegin(), line.rend(), [l = std::locale{}](auto ch) { return !std::isspace(ch, l); }).base(), line.end());
		// removes all space
		//line.erase(std::remove_if(begin(line), end(line), [l = std::locale{}](auto ch) { return std::isspace(ch, l); }), end(line));
		if (line_count == 1)
		{
			isFastq = (line[0] == FASTQ_HEADER_START);
			isFasta = (line[0] == FASTA_HEADER_START);
		}

		if (count == 4 && isFastq)
		{
			count = 0;
		}

		// fastq: 0(header), 1(seq), 2(+), 3(quality)
		// fasta: 0(header), 1(seq)
		if ((isFasta && line[0] == FASTA_HEADER_START) || (isFastq && count == 0))
		{ // add header -->
			if (!read.isEmpty)
			{ // push previous read object to queue
				++read_count;
				read.init(read_count, kvdb, opts);
				//readQueue.push(read);
				break; // return the read here
			}

			// start new record
			read.clear();
			read.format = isFastq ? Format::FASTQ : Format::FASTA;
			read.header = line;
			read.isEmpty = false;

			count = 0; // FASTA record start
		} // ~if header line
		else
		{ // add sequence -->
			if (isFastq)
			{
				if (count == 2) // line[0] == '+' validation is already by readstats::calculate
					continue;
				if (count == 3)
				{
					read.quality = line;
					continue;
				}
			}

			read.sequence += line; // FASTA multi-line sequence or FASTQ sequence
		}
	} // ~for getline
	return read;
} // ~Reader::nextread