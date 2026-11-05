/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_TEXT_DAAT_ITER_H_
#define OB_TEXT_DAAT_ITER_H_

#include "query/ob_sql_define.h"
#include "ob_inv_idx_param_estimator.h"
#include "ob_sparse_bmw_iter.h"
#include "ob_sparse_daat_iter.h"
#include "ob_text_retrieval_token_iter.h"

namespace oceanbase
{
namespace sql
{
  class ObDASScanIter;
}
namespace storage
{

struct ObTextDaaTParam
{
  ObTextDaaTParam()
    : dim_iters_(nullptr), base_param_(nullptr), allocator_(nullptr), relevance_collector_(nullptr),
      bm25_param_est_ctx_(), mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false), cardinality_only_(false) {}
  ~ObTextDaaTParam() {}
  TO_STRING_KV(K_(base_param), KP_(dim_iters), K_(bm25_param_est_ctx),  K_(mode_flag), K_(function_lookup_mode));
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSparseRetrievalMergeParam *base_param_;
  common::ObArenaAllocator *allocator_;
  ObSRDaaTRelevanceCollector *relevance_collector_;
  ObBM25ParamEstCtx bm25_param_est_ctx_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
  bool cardinality_only_;
};

class ObTextDaaTIter final : public ObSRDaaTIterImpl
{
public:
  ObTextDaaTIter() : ObSRDaaTIterImpl(),
      bm25_param_estimator_(),
      mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false),
      batch_merge_enabled_(false),
      cardinality_only_(false),
      batch_ended_mask_(0),
      batch_row_count_(0),
      batch_row_pos_(0),
      batch_buffer_idx_(0),
      batch_id_cmp_() {}
  virtual ~ObTextDaaTIter() { reset(); }

  int init(const ObTextDaaTParam &param);
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
protected:
  virtual int pre_process() override;
  int fill_merge_window();
protected:
  struct BatchMergeRow {
    BatchMergeRow() : id_(nullptr), relevance_(0.0) {}
    TO_STRING_KV(KP_(id), K_(relevance));
    const ObDocIdExt *id_;
    double relevance_;
  };
  static const int64_t MAX_BATCH_MERGE_DIMS = 8;
  ObBM25ParamEstimator bm25_param_estimator_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
  bool batch_merge_enabled_;
  bool cardinality_only_;
  uint64_t batch_ended_mask_;
  int64_t batch_row_count_;
  int64_t batch_row_pos_;
  int64_t batch_buffer_idx_;
  ObFixedArray<BatchMergeRow, ObIAllocator> batch_rows_[2];
  ObDomainIdCmp batch_id_cmp_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextDaaTIter);
};


class ObTextBMWIter final : public ObSRBMWIterImpl
{
public:
  ObTextBMWIter();
  virtual ~ObTextBMWIter() { reset(); }
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  int init(const ObTextDaaTParam &param);
protected:
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  virtual int init_before_wand_process() override;
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextBMWIter);
};

} // namespace storage
} // namespace oceanbase

#endif
