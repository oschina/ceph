// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 * 
 */

#include "common/debug.h"
#include "ErasureCodeJerasure.h"
#include "crush/CrushWrapper.h"
#include "osd/osd_types.h"
extern "C" {
#include "jerasure.h"
#include "reed_sol.h"
#include "galois.h"
#include "cauchy.h"
#include "liberation.h"
}

#define LARGEST_VECTOR_WORDSIZE 16

#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodeJerasure: ";
}

int ErasureCodeJerasure::create_ruleset(const string &name,
					CrushWrapper &crush,
					ostream *ss) const
{
  int ruleid = crush.add_simple_ruleset(name, ruleset_root, ruleset_failure_domain,
					"indep", pg_pool_t::TYPE_ERASURE, ss);
  if (ruleid < 0)
    return ruleid;
  else
    return crush.get_rule_mask_ruleset(ruleid);
}

void ErasureCodeJerasure::init(const map<string,string> &parameters)
{
  dout(10) << "technique=" << technique << dendl;
  map<string,string>::const_iterator parameter;
  parameter = parameters.find("ruleset-root");
  if (parameter != parameters.end())
    ruleset_root = parameter->second;
  parameter = parameters.find("ruleset-failure-domain");
  if (parameter != parameters.end())
    ruleset_failure_domain = parameter->second;
  ostringstream ss;
  if (parse(parameters, &ss))
    derr << ss.str() << dendl;
  prepare();
}

int ErasureCodeJerasure::parse(const map<std::string,std::string> &parameters,
			       ostream *ss)
{
  int err = ErasureCode::parse(parameters, ss);
  err |= to_int("k", parameters, &k, DEFAULT_K, ss);
  err |= to_int("m", parameters, &m, DEFAULT_M, ss);
  err |= to_int("w", parameters, &w, DEFAULT_W, ss);
  if (chunk_mapping.size() > 0 && (int)chunk_mapping.size() != k + m) {
    *ss << "mapping " << parameters.find("mapping")->second
	<< " maps " << chunk_mapping.size() << " chunks instead of"
	<< " the expected " << k + m << " and will be ignored" << std::endl;
    chunk_mapping.clear();
    err = -EINVAL;
  }
  return err;
}

unsigned int ErasureCodeJerasure::get_chunk_size(unsigned int object_size) const
{
  unsigned alignment = get_alignment();
  if (per_chunk_alignment) {
    unsigned chunk_size = object_size / k;
    if (object_size % k)
      chunk_size++;
    dout(20) << "get_chunk_size: chunk_size " << chunk_size
	     << " must be modulo " << alignment << dendl; 
    assert(alignment <= chunk_size);
    unsigned modulo = chunk_size % alignment;
    if (modulo) {
      dout(10) << "get_chunk_size: " << chunk_size
	       << " padded to " << chunk_size + alignment - modulo << dendl;
      chunk_size += alignment - modulo;
    }
    return chunk_size;
  } else {
    unsigned tail = object_size % alignment;
    unsigned padded_length = object_size + ( tail ?  ( alignment - tail ) : 0 );
    assert(padded_length % k == 0);
    return padded_length / k;
  }
}

int ErasureCodeJerasure::encode_chunks(const set<int> &want_to_encode,
				       map<int, bufferlist> *encoded)
{
  char *chunks[k + m];
  for (int i = 0; i < k + m; i++)
    chunks[i] = (*encoded)[i].c_str();
  jerasure_encode(&chunks[0], &chunks[k], (*encoded)[0].length());
  return 0;
}

int ErasureCodeJerasure::decode_chunks(const set<int> &want_to_read,
				       const map<int, bufferlist> &chunks,
				       map<int, bufferlist> *decoded)
{
  unsigned blocksize = (*chunks.begin()).second.length();
  int erasures[k + m + 1];
  int erasures_count = 0;
  char *data[k];
  char *coding[m];
  for (int i =  0; i < k + m; i++) {
    if (chunks.find(i) == chunks.end()) {
      erasures[erasures_count] = i;
      erasures_count++;
    }
    if (i < k)
      data[i] = (*decoded)[i].c_str();
    else
      coding[i - k] = (*decoded)[i].c_str();
  }
  erasures[erasures_count] = -1;

  assert(erasures_count > 0);
  return jerasure_decode(erasures, data, coding, blocksize);
}

bool ErasureCodeJerasure::is_prime(int value)
{
  int prime55[] = {
    2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,
    73,79,83,89,97,101,103,107,109,113,127,131,137,139,149,
    151,157,163,167,173,179,
    181,191,193,197,199,211,223,227,229,233,239,241,251,257
  };
  int i;
  for (i = 0; i < 55; i++)
    if (value == prime55[i])
      return true;
  return false;
}

// 
// ErasureCodeJerasureReedSolomonVandermonde
//
void ErasureCodeJerasureReedSolomonVandermonde::jerasure_encode(char **data,
                                                                char **coding,
                                                                int blocksize)
{
  jerasure_matrix_encode(k, m, w, matrix, data, coding, blocksize);
}

int ErasureCodeJerasureReedSolomonVandermonde::jerasure_decode(int *erasures,
                                                                char **data,
                                                                char **coding,
                                                                int blocksize)
{
  return jerasure_matrix_decode(k, m, w, matrix, 1,
				erasures, data, coding, blocksize);
}

unsigned ErasureCodeJerasureReedSolomonVandermonde::get_alignment() const
{
  if (per_chunk_alignment) {
    return w * LARGEST_VECTOR_WORDSIZE;
  } else {
    unsigned alignment = k*w*sizeof(int);
    if ( ((w*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
      alignment = k*w*LARGEST_VECTOR_WORDSIZE;
    return alignment;
  }
}

int ErasureCodeJerasureReedSolomonVandermonde::parse(const map<std::string,std::string> &parameters,
						      ostream *ss)
{
  int err = ErasureCodeJerasure::parse(parameters, ss);
  if (w != 8 && w != 16 && w != 32) {
    *ss << "ReedSolomonVandermonde: w=" << w
	<< " must be one of {8, 16, 32} : revert to DEFAULT_W " << std::endl;
    w = DEFAULT_W;
    err = -EINVAL;
  }
  err |= to_bool("jerasure-per-chunk-alignement", parameters,
		 &per_chunk_alignment, false, ss);
  return err;
}

void ErasureCodeJerasureReedSolomonVandermonde::prepare()
{
  matrix = reed_sol_vandermonde_coding_matrix(k, m, w);
}

// 
// ErasureCodeJerasureReedSolomonRAID6
//
void ErasureCodeJerasureReedSolomonRAID6::jerasure_encode(char **data,
                                                                char **coding,
                                                                int blocksize)
{
  reed_sol_r6_encode(k, w, data, coding, blocksize);
}

int ErasureCodeJerasureReedSolomonRAID6::jerasure_decode(int *erasures,
							 char **data,
							 char **coding,
							 int blocksize)
{
  return jerasure_matrix_decode(k, m, w, matrix, 1, erasures, data, coding, blocksize);
}

unsigned ErasureCodeJerasureReedSolomonRAID6::get_alignment() const
{
  if (per_chunk_alignment) {
    return w * LARGEST_VECTOR_WORDSIZE;
  } else {
    unsigned alignment = k*w*sizeof(int);
    if ( ((w*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
      alignment = k*w*LARGEST_VECTOR_WORDSIZE;
    return alignment;
  }
}

int ErasureCodeJerasureReedSolomonRAID6::parse(const map<std::string,std::string> &parameters,
					       ostream *ss)
{
  int err = ErasureCodeJerasure::parse(parameters, ss);
  m = 2;
  if (w != 8 && w != 16 && w != 32) {
    *ss << "ReedSolomonRAID6: w=" << w
	<< " must be one of {8, 16, 32} : revert to 8 " << std::endl;
    w = 8;
    err = -EINVAL;
  }
  return err;
}

void ErasureCodeJerasureReedSolomonRAID6::prepare()
{
  matrix = reed_sol_r6_coding_matrix(k, w);
}

// 
// ErasureCodeJerasureCauchy
//
void ErasureCodeJerasureCauchy::jerasure_encode(char **data,
						char **coding,
						int blocksize)
{
  jerasure_schedule_encode(k, m, w, schedule,
			   data, coding, blocksize, packetsize);
}

int ErasureCodeJerasureCauchy::jerasure_decode(int *erasures,
					       char **data,
					       char **coding,
					       int blocksize)
{
  return jerasure_schedule_decode_lazy(k, m, w, bitmatrix,
				       erasures, data, coding, blocksize, packetsize, 1);
}

unsigned ErasureCodeJerasureCauchy::get_alignment() const
{
  if (per_chunk_alignment) {
    unsigned alignment = w * packetsize;
    unsigned modulo = alignment % LARGEST_VECTOR_WORDSIZE;
    if (modulo)
      alignment += LARGEST_VECTOR_WORDSIZE - modulo;
    return alignment;
  } else {
    unsigned alignment = k*w*packetsize*sizeof(int);
    if ( ((w*packetsize*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
      alignment = k*w*packetsize*LARGEST_VECTOR_WORDSIZE;
    return alignment;
  }  
}

int ErasureCodeJerasureCauchy::parse(const map<std::string,std::string> &parameters,
				     ostream *ss)
{
  int err = ErasureCodeJerasure::parse(parameters, ss);
  if (w != 8 && w != 16 && w != 32) {
    *ss << "Cauchy: w=" << w
	<< " must be one of {8, 16, 32} : revert to " 
        << DEFAULT_W << std::endl;
    w = DEFAULT_W;
    err = -EINVAL;
  }
  err |= to_int("packetsize", parameters, &packetsize, DEFAULT_PACKETSIZE, ss);
  err |= to_bool("jerasure-per-chunk-alignement", parameters,
		 &per_chunk_alignment, false, ss);
  return err;
}

void ErasureCodeJerasureCauchy::prepare_schedule(int *matrix)
{
  bitmatrix = jerasure_matrix_to_bitmatrix(k, m, w, matrix);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}

// 
// ErasureCodeJerasureCauchyOrig
//
void ErasureCodeJerasureCauchyOrig::prepare()
{
  int *matrix = cauchy_original_coding_matrix(k, m, w);
  prepare_schedule(matrix);
  free(matrix);
}

// 
// ErasureCodeJerasureCauchyGood
//
void ErasureCodeJerasureCauchyGood::prepare()
{
  int *matrix = cauchy_good_general_coding_matrix(k, m, w);
  prepare_schedule(matrix);
  free(matrix);
}

// 
// ErasureCodeJerasureLiberation
//
ErasureCodeJerasureLiberation::~ErasureCodeJerasureLiberation()
{
  if (bitmatrix)
    free(bitmatrix);
  if (schedule)
    jerasure_free_schedule(schedule);
}

void ErasureCodeJerasureLiberation::jerasure_encode(char **data,
                                                    char **coding,
                                                    int blocksize)
{
  jerasure_schedule_encode(k, m, w, schedule, data,
			   coding, blocksize, packetsize);
}

int ErasureCodeJerasureLiberation::jerasure_decode(int *erasures,
                                                    char **data,
                                                    char **coding,
                                                    int blocksize)
{
  return jerasure_schedule_decode_lazy(k, m, w, bitmatrix, erasures, data,
				       coding, blocksize, packetsize, 1);
}

unsigned ErasureCodeJerasureLiberation::get_alignment() const
{
  unsigned alignment = k*w*packetsize*sizeof(int);
  if ( ((w*packetsize*sizeof(int))%LARGEST_VECTOR_WORDSIZE) )
    alignment = k*w*packetsize*LARGEST_VECTOR_WORDSIZE;
  return alignment;
}

int ErasureCodeJerasureLiberation::parse(const map<std::string,std::string> &parameters,
					 ostream *ss)
{
  int err = ErasureCodeJerasure::parse(parameters, ss);
  err |= to_int("packetsize", parameters, &packetsize, DEFAULT_PACKETSIZE, ss);

  bool error = false;
  if (k > w) {
    *ss << "k=" << k << " must be less than or equal to w=" << w << std::endl;
    error = true;
  }
  if (w <= 2 || !is_prime(w)) {
    *ss <<  "w=" << w << " must be greater than two and be prime" << std::endl;
    error = true;
  }
  if (packetsize == 0) {
    *ss << "packetsize=" << packetsize << " must be set" << std::endl;
    error = true;
  }
  if ((packetsize%(sizeof(int))) != 0) {
    *ss << "packetsize=" << packetsize
	<< " must be a multiple of sizeof(int) = " << sizeof(int) << std::endl;
    error = true;
  }
  if (error) {
    *ss << "reverting to k=" << DEFAULT_K << ", w="
	<< DEFAULT_W << ", packetsize=" << DEFAULT_PACKETSIZE << std::endl;
    k = DEFAULT_K;
    w = DEFAULT_W;
    packetsize = DEFAULT_PACKETSIZE;
    err = -EINVAL;
  }
  return err;
}

void ErasureCodeJerasureLiberation::prepare()
{
  bitmatrix = liberation_coding_bitmatrix(k, w);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}

// 
// ErasureCodeJerasureBlaumRoth
//
void ErasureCodeJerasureBlaumRoth::prepare()
{
  bitmatrix = blaum_roth_coding_bitmatrix(k, w);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}

// 
// ErasureCodeJerasureLiber8tion
//
int ErasureCodeJerasureLiber8tion::parse(const map<std::string,std::string> &parameters,
					 ostream *ss)
{
  int err = ErasureCodeJerasure::parse(parameters, ss);
  m = DEFAULT_M;
  w = DEFAULT_W;
  err |= to_int("packetsize", parameters, &packetsize, DEFAULT_PACKETSIZE, ss);

  bool error = false;
  if (k > w) {
    *ss << "k=" << k << " must be less than or equal to w=" << w << std::endl;
    error = true;
  }
  if (packetsize == 0) {
    *ss << "packetsize=" << packetsize << " must be set" << std::endl;
    error = true;
  }
  if (error) {
    *ss << "reverting to k=" << DEFAULT_K << ", packetsize="
	<< DEFAULT_PACKETSIZE << std::endl;
    k = DEFAULT_K;
    packetsize = DEFAULT_PACKETSIZE;
    err = -EINVAL;
  }
  return err;
}

void ErasureCodeJerasureLiber8tion::prepare()
{
  bitmatrix = liber8tion_coding_bitmatrix(k);
  schedule = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);
}
