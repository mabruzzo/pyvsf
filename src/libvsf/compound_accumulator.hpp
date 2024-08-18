#ifndef COMPOUND_ACCUMULATOR_H
#define COMPOUND_ACCUMULATOR_H

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>  // std::pair
#include <vector>

namespace detail {

template <typename Tup, class Func, std::size_t countdown>
struct for_each_tuple_entry_ {
  static inline void evaluate(Tup& tuple, Func& f) noexcept {
    auto& elem = std::get<std::tuple_size_v<Tup> - countdown>(tuple);
    f(elem);
    for_each_tuple_entry_<Tup, Func, countdown - 1>::evaluate(tuple, f);
  }
};

template <typename Tup, class Func>
struct for_each_tuple_entry_<Tup, Func, 0> {
  static inline void evaluate(Tup& tuple, Func& f) noexcept {}
};
} /* namespace detail */

/// Apply a functor to all elements of a tuple
template <class T, class UnaryFunction>
constexpr inline void for_each_tuple_entry(T& tuple, UnaryFunction f) {
  detail::for_each_tuple_entry_<T, UnaryFunction,
                                std::tuple_size_v<T>>::evaluate(tuple, f);
}

template <typename T>
struct CopyValsHelper_ {
  CopyValsHelper_(T* data_ptr) : data_ptr_(data_ptr), offset_(0) {}

  template <class AccumCollec>
  void operator()(const AccumCollec& accum_collec) noexcept {
    accum_collec.copy_vals(data_ptr_ + offset_);

    std::vector<std::pair<std::string, std::size_t>> val_props;
    if (std::is_same<T, int64_t>::value) {
      val_props = accum_collec.i64_val_props();
    } else {
      val_props = accum_collec.flt_val_props();
    }

    std::size_t n_spatial_bins = accum_collec.n_spatial_bins();
    for (const auto& [quan_name, elem_per_spatial_bin] : val_props) {
      offset_ += n_spatial_bins * elem_per_spatial_bin;
    }
  }

  T* data_ptr_;
  std::size_t offset_;
};

template <typename AccumCollectionTuple>
class CompoundAccumCollection {
  /// @class    CompoundAccumCollection
  ///
  /// @brief Supports multiple accumulators at the same time. This is something
  ///    of a stopgap solution.

public:
  static constexpr std::size_t n_accum =
      std::tuple_size_v<AccumCollectionTuple>;

  static_assert(n_accum > 1,
                "CompoundAccumCollection must be composed of 2+ accumulators.");

  CompoundAccumCollection() = delete;

  CompoundAccumCollection(const CompoundAccumCollection&) = default;

  CompoundAccumCollection(AccumCollectionTuple&& accum_collec_tuple) noexcept
      : accum_collec_tuple_(accum_collec_tuple) {}

  inline void add_entry(std::size_t spatial_bin_index, double val) noexcept {
    for_each_tuple_entry(accum_collec_tuple_,
                         [=](auto& e) { e.add_entry(spatial_bin_index, val); });
  }

  inline void add_entry(std::size_t spatial_bin_index, double val,
                        double weight) noexcept {
    for_each_tuple_entry(accum_collec_tuple_, [=](auto& e) {
      e.add_entry(spatial_bin_index, val, weight);
    });
  }

  /// Updates the values of `*this` to include the values from `other`
  inline void consolidate_with_other(
      const CompoundAccumCollection& other) noexcept {
    auto func = [&](auto& accum_elem) {
      using T = std::decay_t<decltype(accum_elem)>;
      const T& other_accum_elem = std::get<T>(other.accum_collec_tuple_);
      accum_elem.consolidate_with_other(other_accum_elem);
    };
    for_each_tuple_entry(accum_collec_tuple_, func);
  }

  /// Copies the int64_t values of each accumulator to an external buffer
  template <typename T>
  void copy_vals(T* out_vals) const noexcept {
    if constexpr (std::is_same_v<T, std::int64_t>) {
      for_each_tuple_entry(accum_collec_tuple_, CopyValsHelper_(out_vals));
    } else if constexpr (std::is_same_v<T, double>) {
      for_each_tuple_entry(accum_collec_tuple_, CopyValsHelper_(out_vals));
    } else {
      static_assert(dummy_false_v_<T>,
                    "template T must be double or std::int64_t");
    }
  }

  /// Dummy method that needs to be defined to match interface
  static std::vector<std::pair<std::string, std::size_t>>
  flt_val_props() noexcept {
    error("Not Implemented");
  }

  /// Dummy method that needs to be defined to match interface
  std::vector<std::pair<std::string, std::size_t>> i64_val_props() noexcept {
    error("Not Implemented");
  }

  /// Specifies whether the add_entry overload with the weight argument
  /// must be used.
  bool requires_weight() noexcept {
    bool out = false;
    for_each_tuple_entry(accum_collec_tuple_,
                         [&](auto& e) { out = out || e.requires_weight(); });
    return out;
  }

  /// Dummy method that needs to be defined to match interface
  template <typename T>
  void import_vals(const T* in_vals) noexcept {
    error("Not Implemented");
  }

private:
  AccumCollectionTuple accum_collec_tuple_;
};

#endif /* COMPOUND_ACCUMULATOR_H */
