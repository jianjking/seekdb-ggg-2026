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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
namespace
{
inline int compare_batch_doc_id(const ObDatum &lhs, const ObDatum &rhs,
                               const ObDomainIdCmp &id_cmp, const bool binary_id, int &cmp)
{
  int ret = OB_SUCCESS;
  if (binary_id && !lhs.is_null() && !rhs.is_null() && !lhs.is_ext() && !rhs.is_ext()
      && lhs.len_ == 16 && rhs.len_ == 16) {
    // Keep the fixed-width binary comparison inside the batch merge loop.
    cmp = MEMCMP(lhs.ptr_, rhs.ptr_, 16);
  } else {
    ret = id_cmp.compare(lhs, rhs, cmp);
  }
  return ret;
}
}

int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    batch_merge_enabled_ = mode_flag_ == sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
        && !function_lookup_mode_ && iter_param_->eval_ctx_->is_vectorized()
        && relevance_collector_->supports_batched_union()
        && iter_param_->need_project_relevance() && dim_iters_->count() >= 2
        && dim_iters_->count() <= MAX_BATCH_MERGE_DIMS;
    cardinality_only_ = param.cardinality_only_ && batch_merge_enabled_
        && iter_param_->relevance_expr_ == nullptr && !iter_param_->need_filter()
        && !iter_param_->limit_param_->is_valid();
    if (batch_merge_enabled_) {
      const int64_t capacity = dim_iters_->count() * iter_param_->eval_ctx_->max_batch_size_;
      if (OB_FAIL(batch_id_cmp_.init(iter_param_->id_proj_expr_->obj_meta_))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < 2; ++i) {
        batch_rows_[i].set_allocator(iter_allocator_);
        if (OB_FAIL(batch_rows_[i].init(capacity))) {
        } else if (OB_FAIL(batch_rows_[i].prepare_allocate(capacity))) {
        }
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  batch_rows_[0].reset();
  batch_rows_[1].reset();
  batch_id_cmp_.reset();
  batch_merge_enabled_ = false;
  cardinality_only_ = false;
  batch_ended_mask_ = 0;
  batch_row_count_ = 0;
  batch_row_pos_ = 0;
  batch_buffer_idx_ = 0;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  batch_ended_mask_ = 0;
  batch_row_count_ = 0;
  batch_row_pos_ = 0;
  batch_buffer_idx_ = 0;
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::fill_merge_window()
{
  int ret = OB_SUCCESS;
  const ObObjMeta &id_meta = iter_param_->id_proj_expr_->obj_meta_;
  const bool binary_id = id_meta.get_type() == ObVarcharType
      && id_meta.get_collation_type() == CS_TYPE_BINARY;
  const ObDocIdExt *ids[MAX_BATCH_MERGE_DIMS] = {};
  const double *scores[MAX_BATCH_MERGE_DIMS] = {};
  int64_t counts[MAX_BATCH_MERGE_DIMS] = {};
  const ObDocIdExt *watermark = nullptr;
  int cmp = 0;
  for (int64_t dim = 0; OB_SUCC(ret) && dim < dim_iters_->count(); ++dim) {
    if ((batch_ended_mask_ & (1ULL << dim)) != 0) {
    } else if (OB_FAIL(static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(dim))
        ->get_buffered_rows(ids[dim], scores[dim], counts[dim]))) {
      if (ret == OB_ITER_END) {
        batch_ended_mask_ |= 1ULL << dim;
        ret = OB_SUCCESS;
      }
    } else if (watermark == nullptr) {
      watermark = ids[dim] + counts[dim] - 1;
    } else if (OB_FAIL(compare_batch_doc_id(
        ids[dim][counts[dim] - 1].get_datum(), watermark->get_datum(),
        batch_id_cmp_, binary_id, cmp))) {
    } else if (cmp < 0) {
      watermark = ids[dim] + counts[dim] - 1;
    }
  }
  batch_row_count_ = 0;
  batch_row_pos_ = 0;
  if (OB_FAIL(ret)) {
  } else if (watermark == nullptr) {
    ret = OB_ITER_END;
  } else {
    // Every live list is buffered through this ID. No later load can add a
    // contribution to a row in this window; pointers remain valid until output.
    for (int64_t dim = 0; OB_SUCC(ret) && dim < dim_iters_->count(); ++dim) {
      int64_t prefix = 0;
      while (OB_SUCC(ret) && prefix < counts[dim]) {
        if (OB_FAIL(compare_batch_doc_id(
            ids[dim][prefix].get_datum(), watermark->get_datum(),
            batch_id_cmp_, binary_id, cmp))) {
        } else if (cmp > 0) {
          break;
        } else {
          ++prefix;
        }
      }
      BatchMergeRow *input = &batch_rows_[batch_buffer_idx_][0];
      BatchMergeRow *output = &batch_rows_[1 - batch_buffer_idx_][0];
      const bool count_last_union = cardinality_only_ && dim + 1 == dim_iters_->count();
      int64_t i = 0;
      int64_t j = 0;
      int64_t size = 0;
      while (OB_SUCC(ret) && (i < batch_row_count_ || j < prefix)) {
        if (i == batch_row_count_) {
          cmp = 1;
        } else if (j == prefix) {
          cmp = -1;
        } else if (OB_FAIL(compare_batch_doc_id(
            input[i].id_->get_datum(), ids[dim][j].get_datum(),
            batch_id_cmp_, binary_id, cmp))) {
        }
        if (OB_SUCC(ret)) {
          if (cmp < 0) {
            if (!count_last_union) {
              output[size] = input[i];
            }
            ++size;
            ++i;
          } else {
            double relevance = 0.0;
            if (!cardinality_only_) {
              relevance = scores[dim][j];
            }
            if (!cardinality_only_ && iter_param_->dim_weights_ != nullptr) {
              relevance = relevance * iter_param_->field_boost_ * iter_param_->dim_weights_->at(dim);
            }
            if (cmp == 0) {
              relevance = input[i++].relevance_ + relevance;
            }
            if (!count_last_union) {
              output[size].id_ = ids[dim] + j;
              output[size].relevance_ = relevance;
            }
            ++size;
            ++j;
          }
        }
      }
      if (OB_SUCC(ret)) {
        static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(dim))->consume_buffered_rows(prefix);
        batch_buffer_idx_ = 1 - batch_buffer_idx_;
        batch_row_count_ = size;
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!batch_merge_enabled_) {
    ret = ObSRDaaTIterImpl::get_next_rows(capacity, count);
  } else if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (capacity == 0) {
    count = 0;
  } else if (iter_param_->limit_param_->is_valid()
      && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
  } else {
    count = 0;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (batch_row_pos_ == batch_row_count_ && OB_FAIL(fill_merge_window())) {
      } else if (cardinality_only_) {
        // The compiler proved that the scalar COUNT observes cardinality only.
        // All IDs in this window were deduplicated against every live posting.
        const int64_t size = MIN(real_capacity - count, batch_row_count_ - batch_row_pos_);
        batch_row_pos_ += size;
        count += size;
        input_row_cnt_ += size;
        output_row_cnt_ += size;
      } else {
        const BatchMergeRow &row = batch_rows_[batch_buffer_idx_][batch_row_pos_++];
        bool need_project = true;
        if (OB_FAIL(filter_on_demand(count, row.relevance_, need_project))) {
        } else if (need_project && OB_FAIL(cache_result(count, row.id_->get_datum(), row.relevance_))) {
        }
      }
    }
    if (!cardinality_only_ && (OB_SUCC(ret) || ret == OB_ITER_END) && count > 0
        && OB_FAIL(project_results(count))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
