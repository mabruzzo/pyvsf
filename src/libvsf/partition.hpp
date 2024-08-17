// routines to assist with partitioning structure function calculations

#include <algorithm>  // std::min
#include <array>
#include <cstdint>
#include <limits>  // std::numeric_limits
#include <variant>

#include "utils.hpp"  // for error function

template <typename Tend, typename Tstart>
inline Tend safe_cast(Tstart val) noexcept {
  if (val < std::numeric_limits<Tend>::lowest()) {
    if (std::numeric_limits<Tend>::is_signed) {
      error("val is too small to be represented by destination type");
    } else {
      error("can't cast negative val to unsigned type");
    }
  } else if (val > std::numeric_limits<Tend>::max()) {
    error("val is too large to be represented by destination type");
  }
  return static_cast<Tend>(val);
}

struct SlcStruct {
  std::uint64_t start, stop;
};

/// Compute the start and stop indices of a 1D array
///
/// This supports cases where (array_len % num_chunks) != 0
inline SlcStruct calc_chunk_slice(std::size_t chunk_index,
                                  std::size_t array_len,
                                  std::size_t num_chunks) noexcept {
  if ((array_len < num_chunks) | (num_chunks <= chunk_index)) {
    printf("chunk_index = %zu, array_len = %zu, num_chunks = %zu\n",
           chunk_index, array_len, num_chunks);
    fflush(stdout);
    error("something is very wrong");
  }

  auto calc_chunk_size = [=](std::size_t chunk_index) {
    std::size_t remainder = array_len % num_chunks;
    return (array_len / num_chunks) + (chunk_index < remainder);
  };

  // this is a stupid approach, but I've done written this exact function many
  // times. Each time I do it cleverly, I always get it slightly wrong
  std::size_t start_index = 0;
  std::size_t stop_index = 0;
  for (std::size_t cur_chunk_index = 0; cur_chunk_index <= chunk_index;
       cur_chunk_index++) {
    start_index = stop_index;
    stop_index = start_index + calc_chunk_size(cur_chunk_index);
  }

  return {safe_cast<std::uint64_t>(start_index),
          safe_cast<std::uint64_t>(stop_index)};
}

/*  Specifies the number of rectangles and triangles you get when you partition
 *  a distance matrix into s segments.
 *
 *   Consider the following 12 points: [ a b c d e f g h i j k l ]
 *   Consider the following distance matrix for these points is:
 *       [[  ab  ac  ad  ae  af  ag  ah  ai  aj  ak  al]
 *        [   0  bc  bd  be  bf  bg  bh  bi  bj  bk  bl]
 *        [   0   0  cd  ce  cf  cg  ch  ci  cj  ck  cl]
 *        [   0   0   0  de  df  dg  dh  di  dj  dk  dl]
 *        [   0   0   0   0  ef  eg  eh  ei  ej  ek  el]
 *        [   0   0   0   0   0  fg  fh  fi  fj  fk  fl]
 *        [   0   0   0   0   0   0  gh  gi  gj  gk  gl]
 *        [   0   0   0   0   0   0   0  hi  hj  hk  hl]
 *        [   0   0   0   0   0   0   0   0  ij  ik  il]
 *        [   0   0   0   0   0   0   0   0   0  jk  jl]
 *        [   0   0   0   0   0   0   0   0   0   0  kl]
 *
 *   Here's 1 example of partitions into 3 segments per axis:
 *
 *       [[  ab  ac  ad  ae | af  ag  ah  ai | aj  ak  al]
 *        [   0  bc  bd  be | bf  bg  bh  bi | bj  bk  bl]
 *        [   0   0  cd  ce | cf  cg  ch  ci | cj  ck  cl]
 *        [   0   0   0  de | df  dg  dh  di | dj  dk  dl]
 *         ----------------------------------------------
 *        [   0   0   0   0 | ef  eg  eh  ei | ej  ek  el]
 *        [   0   0   0   0 |  0  fg  fh  fi | fj  fk  fl]
 *        [   0   0   0   0 |  0   0  gh  gi | gj  gk  gl]
 *        [   0   0   0   0 |  0   0   0  hi | hj  hk  hl]
 *         ----------------------------------------------
 *        [   0   0   0   0 |  0   0   0   0 | ij  ik  il]
 *        [   0   0   0   0 |  0   0   0   0 |  0  jk  jl]
 *        [   0   0   0   0 |  0   0   0   0 |  0   0  kl]
 *
 *   In detail, you get:
 *   -  3 "triangle" partitions:
 *       [[  ab  ac  ad  ae]    [[  ef  eg  eh  ei]    [[  ij  ik  il]
 *        [   0  bc  bd  be]     [   0  fg  fh  fi]     [   0  jk  jl]
 *        [   0   0  cd  ce]     [   0   0  gh  gi]     [   0   0  kl]]
 *        [   0   0   0  de]]    [   0   0   0  hi]]
 *   - 3 "rectangle" partitions:
 *       [[  af  ag  ah  ai]    [[  aj  ak  al]        [[  ej  ek  el]
 *        [  bf  bg  bh  bi]     [  bj  bk  bl]         [  fj  fk  fl]
 *        [  cf  cg  ch  ci]     [  cj  ck  cl]         [  gj  gk  gl]
 *        [  df  dg  dh  di]]    [  dj  dk  dl]]        [  hj  hk  hl]]
 *
 *   The "triangle" partitions are auto-structure function calculations while
 *   the "rectangle" partitions are cross-structure function calculations.
 *
 *   If we break the columns into s segments and the rows into s segments,
 *   then we will have:
 *       - s "triangle" partitions
 *       - ((s - 1) * s / 2) "rectangle partitions"
 *   In total, we have (s * (s + 1) / 2) segments
 *
 *   For simplicity, we're not going to distinguish between the type of
 *   partition while trying to distribute work. There are (s * (s+1) / 2)
 *
 *   We assign 1D indices to the different chunks as follows:
 *
 *
 *   Here's 1 example of partitions into 3 segments per axis:
 *
 *       [[                 |                |           ]
 *        [ 1D ind: 0       | 1D ind: 1      |1D ind: 2  ]
 *        [ 2D ind: 0,0     | 2D ind: 0,1    |2D ind: 0,2]
 *        [                 |                |           ]
 *         ----------------------------------------------
 *        [   0   0   0   0 |                |           ]
 *        [   0   0   0   0 | 1D ind: 3      |1D ind: 4  ]
 *        [   0   0   0   0 | 2D ind: 1,1    |2D ind: 1,2]
 *        [   0   0   0   0 |                |           ]
 *         ----------------------------------------------
 *        [   0   0   0   0 |  0   0   0   0 |1D ind: 5  ]
 *        [   0   0   0   0 |  0   0   0   0 |2D ind: 2,2]
 *        [   0   0   0   0 |  0   0   0   0 |           ]
 */

std::size_t num_dist_array_chunks_auto(std::size_t segments) {
  // this is a triangle number!
  std::size_t num_triangles = segments;
  std::size_t num_rect = (segments - 1) * segments / 2;
  return num_rect + num_triangles;
}

// When this represents an auto-structure function calculation,
// start_B == stop_B == 0
struct StatTask {
  std::uint64_t start_A, stop_A, start_B, stop_B;
};

struct AutoSFPartitionStrat {
  std::uint64_t n_points;
  std::uint64_t num_segments;

  std::uint64_t n_partitions() const noexcept {
    return num_dist_array_chunks_auto(num_segments);
  }

  void increment2D_index(std::array<std::uint64_t, 2>& index) const noexcept {
    index[1]++;
    if (index[1] == this->num_segments) {
      index[0]++;
      index[1] = index[0];
    }
  }

  StatTask build_StatTask(
      const std::array<std::uint64_t, 2>& index_2D) const noexcept {
    // there's a fairly good chance we have some off-by-1 errors here, we
    // should check that this works

    // reminder: dist_matrix has 1 few entry per axis than this->n_points
    if (this->n_points <= 1) {
      error("not enough points");
    }

    std::uint64_t n_dist_matrix_elements = this->n_points - 1;

    // because of this, we need to tweak start & stop vals for multiple
    // locations
    if (index_2D[0] == index_2D[1]) {  // this is an auto-sf calculation
      SlcStruct tmp = calc_chunk_slice(index_2D[0], n_dist_matrix_elements,
                                       this->num_segments);
      std::uint64_t dist_matrix_start = tmp.start;
      std::uint64_t dist_matrix_stop = tmp.stop;

      return {dist_matrix_start, dist_matrix_stop + 1, 0, 0};

    } else {  // this is a cross-sf calculation
      // points_A and points_B will hold pointers to the same data
      SlcStruct tmp;
      tmp = calc_chunk_slice(index_2D[0], n_dist_matrix_elements,
                             this->num_segments);
      std::uint64_t dist_matrix_start_ax_0 = tmp.start;
      std::uint64_t dist_matrix_stop_ax_0 = tmp.stop;

      tmp = calc_chunk_slice(index_2D[1], n_dist_matrix_elements,
                             this->num_segments);
      std::uint64_t dist_matrix_start_ax_1 = tmp.start;
      std::uint64_t dist_matrix_stop_ax_1 = tmp.stop;

      return {dist_matrix_start_ax_1 + 1, dist_matrix_stop_ax_1 + 1,
              dist_matrix_start_ax_0, dist_matrix_stop_ax_0};
    }
  }

  /// Factory method
  ///
  /// @param skip_small_prob_check when true, this skips a performance check
  ///     that prevents the user from subdividing the problem into partitions
  ///     that are too small
  static AutoSFPartitionStrat create(std::size_t nproc, std::size_t n_points,
                                     bool skip_small_prob_check) noexcept {
    if (nproc == 0) {
      error("nproc can't be zero");
    } else if (n_points <= 1) {
      error("n_points must exceed 1");
    } else if (nproc > 60) {
      error("Probably want to rethink partitioning strategy for so many proc");
    }

    // minimum number of points per segment is 2. For n_points == 5, that means
    // max_segments = 2. For fewer points, force max_segments == 1
    const std::size_t max_segments = (n_points <= 4) ? 1 : (n_points - 1) / 2;

    // our definition of a small problem could be improved
    bool is_small_problem = (!skip_small_prob_check) & (n_points <= 1000);

    if (is_small_problem | (nproc == 1) | (max_segments == 1)) {
      return {safe_cast<std::uint64_t>(n_points), 1};
    }

    // we could definitely use a better algorithm to partition the work more
    // equally. For example, we could count sub triangles and sub rectangles for
    // different amounts of work...

    // determine the number of segments to break each axis of the distance
    // matrix into. The choice of algorithm is fairly arbitrary...
    std::size_t num_segments = 0;
    for (std::size_t cur_num_segments = 2;; cur_num_segments++) {
      if (cur_num_segments >= max_segments) {
        num_segments = max_segments;
        break;
      }
      // compute number of chunks for cur_segments
      std::size_t num_chunks = num_dist_array_chunks_auto(cur_num_segments);
      if (num_chunks >= (3 * nproc)) {
        num_segments = cur_num_segments;
        break;
      }
    }

    if ((num_segments * 2 + 1) > n_points) {  // upper bound to num_segments.
      error("too many segments");
    }

    return {safe_cast<std::uint64_t>(n_points),
            safe_cast<std::uint64_t>(num_segments)};
  }
};

struct CrossSFPartitionStrat {
  std::uint64_t n_points_A;
  std::uint64_t num_segments_A;
  std::uint64_t n_points_B;
  std::uint64_t num_segments_B;

  std::uint64_t n_partitions() const noexcept {
    return num_segments_A * num_segments_B;
  }

  void increment2D_index(std::array<std::uint64_t, 2>& index) const noexcept {
    index[1]++;
    if (index[1] == this->num_segments_B) {
      index[0]++;
      index[1] = 0;
    }
  }

  StatTask build_StatTask(
      const std::array<std::uint64_t, 2>& index_2D) const noexcept {
    if ((index_2D[0] >= num_segments_A) | (index_2D[1] >= num_segments_B)) {
      printf("2D index: (%zu, %zu); effective_shape = (%zu, %zu)\n",
             (std::size_t)index_2D[0], (std::size_t)index_2D[1],
             (std::size_t)num_segments_A, (std::size_t)num_segments_B);
      error("index_2D contains a value that is too large");
    }
    SlcStruct slice_A =
        calc_chunk_slice(index_2D[0], this->n_points_A, this->num_segments_A);
    SlcStruct slice_B =
        calc_chunk_slice(index_2D[1], this->n_points_B, this->num_segments_B);
    return {slice_A.start, slice_A.stop, slice_B.start, slice_B.stop};
  }

  /// Factory method
  ///
  /// @param skip_small_prob_check when true, this skips a performance check
  ///     that prevents the user from subdividing the problem into partitions
  ///     that are too small
  static CrossSFPartitionStrat create(std::size_t nproc, std::size_t n_points_A,
                                      std::size_t n_points_B,
                                      bool skip_small_problem_check) noexcept {
    if (nproc == 0) {
      error("nproc can't be zero");
    }

    // we could definitely use a better algorithm to partition the work more
    // equally (and more conciously of the cache)

    const std::size_t small_npairs = 1000;
    bool exceed_small_npairs =  // try to work around an overflow
        (((n_points_A * n_points_B) > small_npairs) |
         ((n_points_A >= small_npairs) & (n_points_B > 0)) |
         ((n_points_B >= small_npairs) & (n_points_A > 0)));

    // our definition of a small problem could be improved
    bool is_small_problem = !exceed_small_npairs & !skip_small_problem_check;

    if (is_small_problem | ((nproc > n_points_A) & (nproc > n_points_B))) {
      return {safe_cast<std::uint64_t>(n_points_A), 1,
              safe_cast<std::uint64_t>(n_points_B), 1};
    }

    auto builder = [=](bool partition_A) -> CrossSFPartitionStrat {
      return {safe_cast<std::uint64_t>(n_points_A),
              (partition_A) ? safe_cast<std::uint64_t>(nproc) : 1,
              safe_cast<std::uint64_t>(n_points_B),
              (partition_A) ? 1 : safe_cast<std::uint64_t>(nproc)};
    };

    const bool partition_A = true;
    const bool partition_B = false;
    bool smaller_than_both = ((nproc <= n_points_A) & (nproc <= n_points_B));

    if ((nproc <= n_points_A) & ((n_points_A % nproc) == 0)) {
      return builder(partition_A);  // n_points_A is a multiple of n_proc
    } else if ((nproc <= n_points_B) & ((n_points_B % nproc) == 0)) {
      return builder(partition_B);  // n_points_B is a multiple of n_proc
    } else if ((smaller_than_both) & (n_points_B > n_points_A)) {
      return builder(partition_B);
    } else if (smaller_than_both) {  // n_points_B <= n_points_A
      return builder(partition_A);
    } else if (nproc < n_points_B) {
      return builder(partition_B);
    } else {
      return builder(partition_A);
    }
  }
};

using partition_variant =
    std::variant<AutoSFPartitionStrat, CrossSFPartitionStrat>;

class TaskIt {
public:
  TaskIt() = delete;

  template <typename StratT>
  TaskIt(std::uint64_t index_start_1D, std::uint64_t index_stop_1D,
         StratT partition_strat) noexcept
      : index_stop_1D_(index_stop_1D),
        partition_strat_(partition_strat),
        next_index_1D_(),
        next_index_2D_() {
    if (index_stop_1D <= index_start_1D) {
      error("index_stop_1D must exceed index_start_1D");
    }

    // approach for initializing next_index_1D_ & next_index_2D_ is inefficient
    next_index_1D_ = 0;
    next_index_2D_ = {0, 0};
    while (next_index_1D_ < index_start_1D) {
      increment_index_();
    }
  }

  bool has_next() const noexcept { return next_index_1D_ < index_stop_1D_; }

  StatTask next() noexcept {
    auto func = [&](const auto& strat) {
      return strat.build_StatTask(next_index_2D_);
    };

    StatTask out = std::visit(func, partition_strat_);
    increment_index_();
    return out;
  }

private:
  inline void increment_index_() noexcept {
    auto func = [&](const auto& strat) {
      return strat.increment2D_index(next_index_2D_);
    };
    next_index_1D_++;
    std::visit(func, partition_strat_);
  }

private:  // attributes
  const std::uint64_t index_stop_1D_;
  const partition_variant partition_strat_;

  std::uint64_t next_index_1D_;
  std::array<std::uint64_t, 2> next_index_2D_;
};

// only for testing purposes!
inline TaskIt* build_TaskIt_crossSF(std::uint64_t index_start_1D,
                                    std::uint64_t index_stop_1D,
                                    std::uint64_t n_points_A,
                                    std::uint64_t num_segments_A,
                                    std::uint64_t n_points_B,
                                    std::uint64_t num_segments_B) {
  CrossSFPartitionStrat tmp = {n_points_A, num_segments_A, n_points_B,
                               num_segments_B};
  TaskIt* out = new TaskIt(index_start_1D, index_stop_1D, tmp);
  return out;
}

class TaskItFactory {
public:
  TaskItFactory() = delete;

  /// Constructor.
  ///
  /// Pass n_points_other = 0 to indicate an auto structure function calculation
  TaskItFactory(std::size_t nproc, std::size_t n_points,
                std::size_t n_points_other,
                bool skip_small_prob_check = false) noexcept
      : nproc_(nproc),
        partition_strat_(TaskItFactory::build_strat_(
            nproc, n_points, n_points_other, skip_small_prob_check)) {}

  /// gives total number of chunks the problem is broken into
  std::uint64_t n_partitions() const noexcept {
    return std::visit([](const auto& strat) { return strat.n_partitions(); },
                      partition_strat_);
  }

  std::size_t effective_nproc() const noexcept {
    return std::min(nproc_, safe_cast<std::size_t>(n_partitions()));
  }

  /// Constructs the TaskIt for the given process id
  TaskIt build_TaskIt(std::size_t proc_id) const noexcept {
    if (proc_id >= nproc_) {
      error("proc_id is too large");
    }

    // printf("Compute slc before constructing TaskIt\n");
    SlcStruct slc = calc_chunk_slice(
        proc_id, safe_cast<std::size_t>(n_partitions()), effective_nproc());
    // printf("Done computing slc\n");
    return TaskIt(safe_cast<std::uint64_t>(slc.start),
                  safe_cast<std::uint64_t>(slc.stop), partition_strat_);
  }

  /// purely for testing with Cython
  TaskIt* build_TaskIt_ptr(std::size_t proc_id) const noexcept {
    return new TaskIt(build_TaskIt(proc_id));
  }

private:
  static partition_variant build_strat_(std::size_t nproc, std::size_t n_points,
                                        std::size_t n_points_other,
                                        bool skip_small_prob_check) noexcept {
    if (n_points_other == 0) {
      return AutoSFPartitionStrat::create(nproc, n_points,
                                          skip_small_prob_check);
    } else {
      return CrossSFPartitionStrat::create(nproc, n_points, n_points_other,
                                           skip_small_prob_check);
    }
  }

private:
  std::size_t nproc_;
  partition_variant partition_strat_;
};
