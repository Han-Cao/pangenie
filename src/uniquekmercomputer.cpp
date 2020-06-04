#include "uniquekmercomputer.hpp"
#include <jellyfish/mer_dna.hpp>
#include <iostream>
#include <map>

using namespace std;

void unique_kmers(DnaSequence& allele, unsigned char index, size_t kmer_size, map<jellyfish::mer_dna, vector<unsigned char>>& occurences) {
	//enumerate kmers
	map<jellyfish::mer_dna, size_t> counts;
	size_t extra_shifts = kmer_size;
	jellyfish::mer_dna::k(kmer_size);
	jellyfish::mer_dna current_kmer("");
	for (size_t i = 0; i < allele.size(); ++i) {
		char current_base = allele[i];
		if (extra_shifts == 0) {
			counts[current_kmer] += 1;
		}
		if (  ( current_base != 'A') && (current_base != 'C') && (current_base != 'G') && (current_base != 'T') ) {
			extra_shifts = kmer_size + 1;
		}
		current_kmer.shift_left(current_base);
		if (extra_shifts > 0) extra_shifts -= 1;
	}
	counts[current_kmer] += 1;

	// determine kmers unique to allele
	for (auto const& entry : counts) {
		if (entry.second == 1) occurences[entry.first].push_back(index);
	}
}

double get_error_parameter(double kmer_coverage) {
	double cn0;
	if (kmer_coverage < 10.0) {
		cn0 = 0.99;
	} else if (kmer_coverage < 20) {
		cn0 = 0.95;
	} else if (kmer_coverage < 40) {
		cn0 = 0.9;
	} else {
		cn0 = 0.8;
	}
	return cn0;
}

UniqueKmerComputer::UniqueKmerComputer (KmerCounter* genomic_kmers, KmerCounter* read_kmers, VariantReader* variants, string chromosome, size_t kmer_coverage)
	:genomic_kmers(genomic_kmers),
	 read_kmers(read_kmers),
	 variants(variants),
	 chromosome(chromosome),
	 kmer_coverage(kmer_coverage)
{
	jellyfish::mer_dna::k(this->variants->get_kmer_size());
}

void UniqueKmerComputer::compute_unique_kmers(vector<UniqueKmers*>* result, long double regularization_const) {
	size_t nr_variants = this->variants->size_of(this->chromosome);
	for (size_t v = 0; v < nr_variants; ++v) {

		// set parameters of distributions
		size_t kmer_size = this->variants->get_kmer_size();
		double kmer_coverage = compute_local_coverage(this->chromosome, v, 2*kmer_size);
		double cn0 = get_error_parameter(kmer_coverage);
		double cn1 = kmer_coverage / 2.0;
		double cn2 = kmer_coverage;
		this->probability.set_parameters(cn0, cn1, cn2);
		
		map <jellyfish::mer_dna, vector<unsigned char>> occurences;
		const Variant& variant = this->variants->get_variant(this->chromosome, v);
		UniqueKmers* u = new UniqueKmers(v, variant.get_start_position());
		u->set_coverage(kmer_coverage);
		size_t nr_alleles = variant.nr_of_alleles();

		// insert empty alleles (to also capture paths for which no unique kmers exist)
		for (size_t p = 0; p < variant.nr_of_paths(); ++p) {
			unsigned char a = variant.get_allele_on_path(p);
			u->insert_empty_allele(a);
			u->insert_path(p,a);
		}

		// whether any of the alleles is undefined
		bool any_undefined = false;
		for (unsigned char a = 0; a < nr_alleles; ++a) {
			// enumerate kmers and identify those with copynumber 1
			DnaSequence allele = variant.get_allele_sequence(a);
			if (allele.contains_undefined()) {
				any_undefined = true;
				break;
			}
			unique_kmers(allele, a, kmer_size, occurences);
		}

		// TODO: for now, if any allele is undefined, we don't consider any kmers
		occurences.clear();

		// check if kmers occur elsewhere in the genome
		size_t nr_kmers_used = 0;
		for (auto& kmer : occurences) {
			if (nr_kmers_used > 300) break;

			size_t genomic_count = this->genomic_kmers->getKmerAbundance(kmer.first);
			size_t local_count = kmer.second.size();

			if ( (genomic_count - local_count) == 0 ) {
				// kmer unique to this region
				// determine read kmercount for this kmer
				size_t read_kmercount = this->read_kmers->getKmerAbundance(kmer.first);

				// determine on which paths kmer occurs
				vector<size_t> paths;
				for (auto& allele : kmer.second) {
					variant.get_paths_of_allele(allele, paths);
				}

				// skip kmer that does not occur on any path (uncovered allele)
				if (paths.size() == 0) {
					continue;
				}

				// skip kmer that occurs on all paths (they do not give any information about a genotype)
				if (paths.size() == variant.nr_of_paths()) {
					continue;
				}

				// skip kmers with "too extreme" counts
				// TODO: value ok?
				if (read_kmercount > (2*this->kmer_coverage)) {
					continue;
				}

				// determine probabilities
				long double p_cn0 = this->probability.get_probability(0, read_kmercount);
				long double p_cn1 = this->probability.get_probability(1, read_kmercount);
				long double p_cn2 = this->probability.get_probability(2, read_kmercount);

				// skip kmers with only 0 probabilities
				if ( (p_cn0 > 0) || (p_cn1 > 0) || (p_cn2 > 0) ) {
					nr_kmers_used += 1;
					if (regularization_const > 0) {
						CopyNumber cn(p_cn0, p_cn1, p_cn2, regularization_const);
						u->insert_kmer(cn, kmer.second);
					} else {
						// not normalizing seems to increase precision
						CopyNumber cn(p_cn0, p_cn1, p_cn2);
						u->insert_kmer(cn, kmer.second);
					}
				}
			}
		}
		result->push_back(u);
	}
}

void UniqueKmerComputer::compute_empty(vector<UniqueKmers*>* result) const {
	size_t nr_variants = this->variants->size_of(this->chromosome);
	for (size_t v = 0; v < nr_variants; ++v) {
		const Variant& variant = this->variants->get_variant(this->chromosome, v);
		UniqueKmers* u = new UniqueKmers(v, variant.get_start_position());
		size_t nr_alleles = variant.nr_of_alleles();

		// insert empty alleles and paths
		for (size_t p = 0; p < variant.nr_of_paths(); ++p) {
			unsigned char a = variant.get_allele_on_path(p);
			u->insert_empty_allele(a);
			u->insert_path(p,a);
		}
		result->push_back(u);
	}
}

double UniqueKmerComputer::compute_local_coverage(string chromosome, size_t var_index, size_t length) {
	DnaSequence left_overhang;
	DnaSequence right_overhang;
	double total_coverage = 0;
	double total_kmers = 0;

	this->variants->get_left_overhang(chromosome, var_index, length, left_overhang);
	this->variants->get_right_overhang(chromosome, var_index, length, right_overhang);

	size_t kmer_size = this->variants->get_kmer_size();
	map <jellyfish::mer_dna, vector<unsigned char>> occurences;
	unique_kmers(left_overhang, 0, kmer_size, occurences);
	unique_kmers(right_overhang, 1, kmer_size, occurences);

	for (auto& kmer : occurences) {
		size_t genomic_count = this->genomic_kmers->getKmerAbundance(kmer.first);
		if (genomic_count == 1) {
			size_t read_count = this->read_kmers->getKmerAbundance(kmer.first);
			// ignore too extreme counts
			if ( (read_count < (this->kmer_coverage/4)) || (read_count > (this->kmer_coverage*4)) ) continue;
			total_coverage += read_count;
			total_kmers += 1;
		}
	}
	// in case no unique kmers were found, use constant kmer coverage
	if ((total_kmers > 0) && (total_coverage > 0)){
		return total_coverage / total_kmers;
	} else {
		return this->kmer_coverage;
	}
}
