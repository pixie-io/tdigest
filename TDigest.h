#ifndef TDIGEST2_TDIGEST_H_
#define TDIGEST2_TDIGEST_H_

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>

#include "glog/logging.h"

namespace tdigest {

using Value = double;
using Weight = double;
using Index = size_t;

class Centroid {
 public:
  Centroid(Value mean, Weight weight) : mean_(mean), weight_(weight) {}

  inline Weight mean() const noexcept { return mean_; }

  inline Weight weight() const noexcept { return weight_; }

  inline void add(const Centroid& c) {
    CHECK_GT(c.weight_, 0);
    weight_ += c.weight_;
    mean_ += c.weight_ * (c.mean_ - mean_) / weight_;
  }

 private:
  Value mean_ = 0;
  Weight weight_ = 0;
};

struct CentroidList {
  CentroidList(const std::vector<Centroid>& s) : iter(s.cbegin()), end(s.cend()) {}
  std::vector<Centroid>::const_iterator iter;
  std::vector<Centroid>::const_iterator end;

  bool advance() { return ++iter != end; }
};

class CentroidListComparator {
 public:
  CentroidListComparator() {}

  bool operator()(const CentroidList& left, const CentroidList& right) const {
    return left.iter->mean() > right.iter->mean();
  }
};

using CentroidListQueue = std::priority_queue<CentroidList, std::vector<CentroidList>, CentroidListComparator>;

struct CentroidComparator {
  bool operator()(const Centroid& a, const Centroid& b) const { return a.mean() < b.mean(); }
};

class TDigest {
 public:
  explicit TDigest(Value compression) : TDigest(compression, 0) {}

  TDigest(Value compression, Index bufferSize) : TDigest(compression, bufferSize, 0) {}

  TDigest(Value compression, Index unmergedSize, Index mergedSize)
      : compression_(compression),
        maxProcessed_(processedSize(mergedSize, compression)),
        maxUnprocessed_(unprocessedSize(unmergedSize, compression)) {
    processed_.reserve(maxProcessed_);
    unprocessed_.reserve(maxUnprocessed_ + 1);  
  }

  TDigest(std::vector<Centroid>&& processed, std::vector<Centroid>&& unprocessed, Value compression,
          Index unmergedSize, Index mergedSize)
      : TDigest(compression, unmergedSize, mergedSize) {
    processed_ = std::move(processed);
    unprocessed_ = std::move(unprocessed);
  }

  static inline Index processedSize(Index size, Value compression) noexcept {
    return (size == 0) ? static_cast<Index>(2 * std::ceil(compression)) : size;
  }

  static inline Index unprocessedSize(Index size, Value compression) noexcept {
    return (size == 0) ? static_cast<Index>(8 * std::ceil(compression)) : size;
  }

  // merge in another t-digest
  inline void merge(const TDigest& other) {
    std::vector<TDigest> others{other};
    add(others);
  }

  const std::vector<Centroid>& processed() { return processed_; }

  const std::vector<Centroid>& unprocessed() { return unprocessed_; }

  Index maxUnprocessed() { return maxUnprocessed_; }

  Index maxProcessed() { return maxProcessed_; }

  void add(const std::vector<TDigest> others) {
    if (others.size() > 0) {
      mergeProcessed(others);
      mergeUnprocessed(others);
      dirty_ = true;
      processIfNecessary();
    }
  }

  long totalWeight() const { return static_cast<long>(processedWeight_ + unprocessedWeight_); }

  // return the cdf on the t-digest
  Value cdf(Value x, bool* success) {
    process();
    return cdfProcessed(x, success);
  }

  // true if there are unprocessed values in the t-digest
  bool haveUnprocessed() const noexcept { return unprocessed_.size() > 0; }

  // return the cdf on the processed values
  Value cdfProcessed(Value x, bool* success) const {
    *success = true;
    if (processed_.size() == 0) {
      // no data to examine
      *success = false;
      return 0.0;
    } else if (processed_.size() == 1) {
      // exactly one centroid, should have max==min
      auto width = max - min;
      if (x < min) {
        return 0.0;
      } else if (x > max) {
        return 1.0;
      } else if (x - min <= width) {
        // min and max are too close together to do any viable interpolation
        return 0.5;
      } else {
        // interpolate if somehow we have weight > 0 and max != min
        return (x - min) / (max - min);
      }
    } else {
      auto n = processed_.size();
      if (x <= min) {
        return 0;
      }

      if (x >= max) {
        return 1;
      }

      // check for the left tail
      if (x <= mean(0)) {
        // note that this is different than mean(0) > min ... this guarantees interpolation works
        if (mean(0) - min > 0) {
          return (x - min) / (mean(0) - min) * weight(0) / processedWeight_ / 2.0;
        } else {
          return 0;
        }
      }
      CHECK_GT(x, mean(0));

      // and the right tail
      if (x >= mean(n - 1)) {
        if (max - mean(n - 1) > 0) {
          return 1.0 - (max - x) / (max - mean(n - 1)) * weight(n - 1) / processedWeight_ / 2.0;
        } else {
          return 1;
        }
      }
      CHECK_LT(x, mean(n - 1));

      // we know that there are at least two sorted and x > mean(0) && x < mean(n-1)
      // that means that there are either a bunch of consecutive sorted all equal at x
      // or there are consecutive sorted, c0 <= x and c1 > x
      auto weightSoFar = weight(0) / 2.0;
      for (size_t it = 0; it < n; it++) {
        if (mean(it) == x) {
          auto w0 = weightSoFar;
          while (it < n && mean(it + 1) == x) {
            weightSoFar += (weight(it) + weight(it + 1));
            it++;
          }
          return (w0 + weightSoFar) / 2 / processedWeight_;
        }
        if (mean(it) <= x && mean(it + 1) > x) {
          if (mean(it + 1) - mean(it) > 0.0) {
            auto dw = (weight(it) + weight(it + 1)) / 2.0;
            return (weightSoFar + dw * (x - mean(it)) / (mean(it + 1) - mean(it))) / processedWeight_;
          } else {
            // this is simply caution against floating point madness
            // it is conceivable that the sorted will be different
            // but too near to allow safe interpolation
            auto dw = (weight(it) + weight(it + 1)) / 2.0;
            return weightSoFar + dw / processedWeight_;
          }
        }
        weightSoFar += (weight(it) + weight(it + 1)) / 2.0;
      }
    }
  }

  // this returns a quantile on the t-digest
  Value quantile(Value q, bool* success) {
    process();
    return quantileProcessed(q, success);
  }

  // this returns a quantile on the currently processed values without changing the t-digest
  // the value will not represent the unprocessed values
  Value quantileProcessed(Value q, bool* success) const {
    if (q < 0 || q > 1) {
      LOG(ERROR) << "q should be in [0,1], got " << q;
      *success = false;
      return 0.0;
    }

    if (processed_.size() == 0) {
      // no sorted means no data, no way to get a quantile
      *success = false;
      return 0.0;
    } else if (processed_.size() == 1) {
      // with one data point, all quantiles lead to Rome
      *success = true;
      return mean(0);
    }

    // we know that there are at least two sorted now
    auto n = processed_.size();

    // if values were stored in a sorted array, index would be the offset we are Weighterested in
    const auto index = q * processedWeight_;

    // at the boundaries, we return min or max
    if (index < weight(0) / 2.0) {
      CHECK_GT(weight(0), 0);
      *success = true;
      return min + 2.0 * index / weight(0) * (mean(0) - min);
    }

    // in between we interpolate between sorted
    auto weightSoFar = weight(0) / 2.0;
    for (Weight i = 0; i < n - 1; i++) {
      auto dw = (weight(i) + weight(i + 1)) / 2.0;
      if (weightSoFar + dw > index) {
        // sorted i and i+1 bracket our current point
        auto z1 = index - weightSoFar;
        auto z2 = weightSoFar + dw - index;
        *success = true;
        return weightedAverage(mean(i), z2, mean(i + 1), z1);
      }
      weightSoFar += dw;
    }
    CHECK_LE(index, processedWeight_);
    CHECK_GE(index, processedWeight_ - weight(n - 1) / 2.0);

    auto z1 = index - processedWeight_ - weight(n - 1) / 2.0;
    auto z2 = weight(n - 1) / 2 - z1;
    *success = true;
    return weightedAverage(mean(n - 1), z1, max, z2);
  }

  Value compression() { return compression_; }

  void add(Value x) { add(x, 1); }

  inline void compress() { processIfNecessary(); }

 private:
  const Value compression_;

  Value min = std::numeric_limits<Value>::max();

  Value max = std::numeric_limits<Value>::min();

  Index maxProcessed_;

  Index maxUnprocessed_;

  Value processedWeight_ = 0.0;

  Value unprocessedWeight_ = 0.0;

  std::vector<Centroid> processed_;

  std::vector<Centroid> unprocessed_;

  bool dirty_ = false;

  // return mean of i-th centroid
  inline Value mean(int i) const noexcept { return processed_[i].mean(); }

  // return weight of i-th centroid
  inline Weight weight(int i) const noexcept { return processed_[i].weight(); }

  // add a single centroid to the unprocessed vector, processing previously unprocessed sorted if our limit has
  // been reached.
  inline bool add(Value x, Weight w) {
    if (std::isnan(x)) {
      return false;
    }
    unprocessed_.push_back(Centroid(x, w));
    unprocessedWeight_ += w;
    dirty_ = true;
    processIfNecessary();
    return true;
  }

  // append all unprocessed centroids into current unprocessed vector
  void mergeUnprocessed(const std::vector<TDigest>& tdigests) {
    if (tdigests.size() == 0) return;

    size_t total = unprocessed_.size();
    for (auto& td : tdigests) {
      total += td.unprocessed_.size();
    }

    unprocessed_.reserve(total);
    for (auto& td : tdigests) {
      unprocessed_.insert(unprocessed_.end(), td.unprocessed_.cbegin(), td.unprocessed_.cend());
      unprocessedWeight_ += td.unprocessedWeight_;
    }
  }

  // merge all processed centroids together into a single sorted vector
  void mergeProcessed(const std::vector<TDigest>& tdigests) {
    if (tdigests.size() == 0) return;

    size_t total = processed_.size();
    CentroidListQueue pq(CentroidListComparator{});
    for (auto& td : tdigests) {
      auto& sorted = td.processed_;
      auto size = sorted.size();
      if (size > 0) {
        pq.push(CentroidList(sorted));
        total += size;
        processedWeight_ += td.processedWeight_;
      }
    }
    pq.push(CentroidList(processed_));

    std::vector<Centroid> sorted;
    sorted.reserve(total);

    while (!pq.empty()) {
      auto best = pq.top();
      pq.pop();
      sorted.push_back(*best.iter);
      if (best.advance()) pq.push(best);
    }
    processed_ = std::move(sorted);
  }

  inline void processIfNecessary() {
    if (unprocessed_.size() > maxUnprocessed_ || processed_.size() > maxProcessed_) {
      process();
    }
  }

  // merges unprocessed_ centroids and processed_ centroids together and processes them
  // when complete, unprocessed_ will be empty and processed_ will have at most maxProcessed_ centroids
  inline void process() {
    if( !dirty_ ) return;
    CentroidComparator cc;
    std::sort(unprocessed_.begin(), unprocessed_.end(), cc);
    auto count = unprocessed_.size();
    unprocessed_.insert(unprocessed_.end(), processed_.cbegin(), processed_.cend());
    std::inplace_merge(unprocessed_.begin(), unprocessed_.begin() + count, unprocessed_.end(), cc);

    processedWeight_ += unprocessedWeight_;
    unprocessedWeight_ = 0;
    processed_.clear();

    processed_.push_back(unprocessed_[0]);
    Weight wSoFar = unprocessed_[0].weight();
    Weight wLimit = processedWeight_ * integratedQ(1.0);

    for (auto& centroid : unprocessed_) {
      Weight projectedW = wSoFar + centroid.weight();
      if (projectedW <= wLimit) {
        wSoFar = projectedW;
        (processed_.end() - 1)->add(centroid);
      } else {
        auto k1 = integratedLocation(wSoFar / processedWeight_);
        wLimit = processedWeight_ * integratedQ(k1 + 1.0);
        wSoFar += centroid.weight();
        processed_.emplace_back(centroid);
      }
    }
    dirty_ = false;
    unprocessed_.clear();
    min = std::min(min, processed_[0].mean());
    max = std::max(max, (processed_.cend() - 1)->mean());
  }

  inline int checkWeights() { return checkWeights(processed_, processedWeight_); }

  int checkWeights(const std::vector<Centroid>& sorted, Value total) {
    Weight badWeight = 0;
    auto k1 = 0.0;
    auto q = 0.0;
    for (auto iter = sorted.cbegin(); iter != sorted.cend(); iter++) {
      auto w = iter->weight();
      auto dq = w / total;
      auto k2 = integratedLocation(q + dq);
      if (k2 - k1 > 1 && w != 1) {
        LOG(WARNING) << "Oversize centroid at " << std::distance(sorted.cbegin(), iter) << " k1 " << k1 << " k2 " << k2
                     << " dk " << (k2 - k1) << " w " << w << " q " << q;
        badWeight++;
      }
      if (k2 - k1 > 1.5 && w != 1) {
        LOG(ERROR) << "Egregiously Oversize centroid at " << std::distance(sorted.cbegin(), iter) << " k1 " << k1
                   << " k2 " << k2 << " dk " << (k2 - k1) << " w " << w << " q " << q;
        badWeight++;
      }
      q += dq;
      k1 = k2;
    }

    return badWeight;
  }

  /**
   * Converts a quantile into a centroid scale value.  The centroid scale is nominally
   * the number k of the centroid that a quantile point q should belong to.  Due to
   * round-offs, however, we can't align things perfectly without splitting points
   * and sorted.  We don't want to do that, so we have to allow for offsets.
   * In the end, the criterion is that any quantile range that spans a centroid
   * scale range more than one should be split across more than one centroid if
   * possible.  This won't be possible if the quantile range refers to a single point
   * or an already existing centroid.
   * <p/>
   * This mapping is steep near q=0 or q=1 so each centroid there will correspond to
   * less q range.  Near q=0.5, the mapping is flatter so that sorted there will
   * represent a larger chunk of quantiles.
   *
   * @param q The quantile scale value to be mapped.
   * @return The centroid scale value corresponding to q.
   */
  inline Value integratedLocation(Value q) const {
    return compression_ * (std::asin(2.0 * q - 1.0) + M_PI / 2) / M_PI;
  }

  inline Value integratedQ(Value k) const {
    return (std::sin(std::min(k, compression_) * M_PI / compression_ - M_PI / 2) + 1) / 2;
  }

  /**
   * Same as {@link #weightedAverageSorted(Value, Value, Value, Value)} but flips
   * the order of the variables if <code>x2</code> is greater than
   * <code>x1</code>.
   */
  static Value weightedAverage(Value x1, Value w1, Value x2, Value w2) {
    return (x1 <= x2) ? weightedAverageSorted(x1, w1, x2, w2) : weightedAverageSorted(x2, w2, x1, w1);
  }

  /**
   * Compute the weighted average between <code>x1</code> with a weight of
   * <code>w1</code> and <code>x2</code> with a weight of <code>w2</code>.
   * This expects <code>x1</code> to be less than or equal to <code>x2</code>
   * and is guaranteed to return a number between <code>x1</code> and
   * <code>x2</code>.
   */
  static Value weightedAverageSorted(Value x1, Value w1, Value x2, Value w2) {
    CHECK_LE(x1, x2);
    const Value x = (x1 * w1 + x2 * w2) / (w1 + w2);
    return std::max(x1, std::min(x, x2));
  }

  static Value interpolate(Value x, Value x0, Value x1) { return (x - x0) / (x1 - x0); }

  /**
   * Computes an interpolated value of a quantile that is between two sorted.
   *
   * Index is the quantile desired multiplied by the total number of samples - 1.
   *
   * @param index              Denormalized quantile desired
   * @param previousIndex      The denormalized quantile corresponding to the center of the previous centroid.
   * @param nextIndex          The denormalized quantile corresponding to the center of the following centroid.
   * @param previousMean       The mean of the previous centroid.
   * @param nextMean           The mean of the following centroid.
   * @return  The interpolated mean.
   */
  static Value quantile(Value index, Value previousIndex, Value nextIndex, Value previousMean, Value nextMean) {
    const auto delta = nextIndex - previousIndex;
    const auto previousWeight = (nextIndex - index) / delta;
    const auto nextWeight = (index - previousIndex) / delta;
    return previousMean * previousWeight + nextMean * nextWeight;
  }
};

}  // namespace tdigest2

#endif  // TDIGEST2_TDIGEST_H_