#ifndef ACCUMULATORS_H
#define ACCUMULATORS_H

#include <utility> // std::pair

[[noreturn]] void error(const char* message){
  if (message == nullptr){
    printf("ERROR\n");
  } else {
    printf("ERROR: %s\n", message);
  }
  exit(1);
}

// The accumulator structs must all satisfy the following properties:
// - static method called ``flt_val_names`` that returns the names of each
//   floating point value
// - instance method called ``get_flt_val`` that returns the stored floating
//   point value corresponding to the name returned by flt_val_names
// - must have a default constructor
// - must define the ``add_entry`` instance method that updates the
//   statistic(s) that are being accumulated.
// - must currently define the count attribute (which tracks the number of
//   entries that have been added to the accumulator so far).

struct MeanAccum{

public: // interface
  static std::vector<std::string> flt_val_names() noexcept{
    return {"mean"};
  }

  double get_flt_val(std::size_t i) const noexcept{
    if (i == 0){
      return mean;
    } else {
      error("MeanAccum only has 1 float_val");
    }
  }

  MeanAccum() : count(0), mean(0.0) {}

  inline void add_entry(double val) noexcept{
    count++;
    double val_minus_last_mean = val - mean;
    mean += (val_minus_last_mean)/count;
  }

public: // attributes
  // number of entries included (so far)
  int64_t count;
  // current mean
  double mean;
};


struct VarAccum {

  public: // interface
  static std::vector<std::string> flt_val_names() noexcept{
    return {"mean", "variance"};
  }

  double get_flt_val(std::size_t i) const noexcept{
    if (i == 0){
      return mean;
    } else if (i == 1){
      return (count > 1) ? cur_M2 / (count - 1) : 0.0;
    } else {
      error("VarAccum only has 2 float_vals");
    }
  }

  VarAccum() : count(0), mean(0.0), cur_M2(0.0) {}

  inline void add_entry(double val) noexcept{
    count++;
    double val_minus_last_mean = val - mean;
    mean += (val_minus_last_mean)/count;
    double val_minus_cur_mean = val - mean;
    cur_M2 += val_minus_last_mean * val_minus_cur_mean;
  }

public: // attributes
  // number of entries included (so far)
  int64_t count;
  // current mean
  double mean;
  // sum of differences from the current mean
  double cur_M2;

};



template<typename Accum>
class ScalarAccumCollection{

public:

  ScalarAccumCollection() noexcept : accum_list_() {}

  ScalarAccumCollection(std::size_t n_spatial_bins, void * other_arg) noexcept
    : accum_list_(n_spatial_bins)
  {
    if (n_spatial_bins == 0) { error("n_spatial_bins must be positive"); }
    if (other_arg != nullptr) { error("other_arg must be nullptr"); }
  }

  inline void add_entry(std::size_t spatial_bin_index, double val) noexcept{
    accum_list_[spatial_bin_index].add_entry(val);
  }

  /// Return the Floating Point Value Properties
  ///
  /// This is a vector holding pairs of the value name and the number of value
  /// entries per spatial bin.
  ///
  /// For Scalar Accumulators, each value only stores 1 entry per spatial bin
  static std::vector<std::pair<std::string,std::size_t>> flt_val_props()
    noexcept
  {
    std::vector<std::string> flt_val_names = Accum::flt_val_names();

    std::vector<std::pair<std::string,std::size_t>> out;
    for (std::size_t i = 0; i < flt_val_names.size(); i++){
      out.push_back(std::make_pair(flt_val_names[i], 1));
    }
    return out;
  }

  /// Return the Int64 Value Properties
  ///
  /// This is a vector holding pairs of the integer value name and the number
  /// of value entries per spatial bin.
  ///
  /// This is currently the same for all scalar accumulators
  static std::vector<std::pair<std::string,std::size_t>> i64_val_props()
    noexcept
  { return {{"count", 1}}; }

  void copy_flt_vals(double *out_vals) noexcept {
    const std::size_t num_flt_vals = Accum::flt_val_names().size();
    const std::size_t n_bins = accum_list_.size();

    for (std::size_t i = 0; i < n_bins; i++){
      for (std::size_t j = 0; j < num_flt_vals; j++){
	out_vals[i + j*n_bins] = accum_list_[i].get_flt_val(j);
      }
    }
  }

  void copy_i64_vals(int64_t *out_vals) noexcept {
    for (std::size_t i = 0; i < accum_list_.size(); i++){
      out_vals[i] = accum_list_[i].count;
    }
  }

private:
  std::vector<Accum> accum_list_;

};

/// identify the index of the bin where x lies.
///
/// @param x The value that is being queried
/// @param bin_edges An array of monotonically increasing bin edges. This
///    must have ``nbins + 1`` entries. The ith bin includes the interval
///    ``bin_edges[i] <= x < bin_edges[i]``.
/// @param nbins The number of bins. This is expected to be at least 1.
///
/// @returns index The index that ``x`` belongs in. If ``x`` doesn't lie in
///    any bins, ``nbins`` is returned.
///
/// @notes
/// At the moment we are using a binary search algorithm. In the future, we
/// might want to assess the significance of branch mispredictions.
template<typename T>
std::size_t identify_bin_index(T x, const T *bin_edges, std::size_t nbins)
{
  const T* bin_edges_end = bin_edges+nbins+1;
  const T* rslt = std::lower_bound(bin_edges, bin_edges_end, x);
  // rslt is a pointer to the first value that is "not less than" x
  std::size_t index_p_1 = std::distance(bin_edges, rslt);

  if (index_p_1 == 0 || index_p_1 == (nbins + 1)){
    return nbins;
  } else {
    return index_p_1 - 1;
  }
}

class HistogramAccumCollection{
public:

  HistogramAccumCollection() noexcept
    : n_spatial_bins_(),
      n_data_bins_(),
      bin_counts_(),
      data_bin_edges_()
  { }
  
  HistogramAccumCollection(std::size_t n_spatial_bins,
                           void * other_arg) noexcept
    : n_spatial_bins_(n_spatial_bins),
      n_data_bins_(),
      bin_counts_(),
      data_bin_edges_()
  {
    if (n_spatial_bins == 0) { error("n_spatial_bins must be positive"); }
    if (other_arg == nullptr) { error("other_arg must not be a nullptr"); }


    BinSpecification* data_bins = static_cast<BinSpecification*>(other_arg);
    //printf("n_bins = %d\n", (int)data_bins->n_bins);
    //for (std::size_t i = 0; i < (data_bins->n_bins + 1); i++){
    //  printf("%e, ", data_bins->bin_edges[i]);
    //}
    //printf("\n");

    //std::size_t n_data_bins = 3;
    //double data_bin_edges[4] = {-1.7976931348623157e+308, 1e-8, 1e-4, 1.7976931348623157e+308};
    // initialize n_data_bins_
    if (data_bins->n_bins == 0) {
      error("There must be a positive number of bins.");
    }
    n_data_bins_ = data_bins->n_bins;

    // initialize data_bin_edges_ (copy data from data_bin_edges)
    std::size_t len_data_bin_edges = n_data_bins_ + 1;
    data_bin_edges_.resize(len_data_bin_edges);
    // we should really confirm the data_bin_edges_ is monotonic
    for (std::size_t i = 0; i < len_data_bin_edges; i++){
      data_bin_edges_[i] = data_bins->bin_edges[i];
    }

    // initialize the counts array
    bin_counts_.resize(n_data_bins_ * n_spatial_bins_, 0);
  }

  inline void add_entry(std::size_t spatial_bin_index, double val) noexcept{
    std::size_t data_bin_index = identify_bin_index(val,
                                                    data_bin_edges_.data(),
                                                    n_data_bins_);
    if (data_bin_index < n_data_bins_){
      std::size_t i = data_bin_index + spatial_bin_index*n_data_bins_;
      bin_counts_[i]++;
    }
  }

  /// Return the Floating Point Value Properties
  ///
  /// This is a vector holding pairs of the value name and the number of value
  /// entries per spatial bin.
  ///
  /// For Scalar Accumulators, each value only stores 1 entry per spatial bin
  static std::vector<std::pair<std::string,std::size_t>> flt_val_props()
    noexcept
  {
    std::vector<std::pair<std::string,std::size_t>> out;
    return out;
  }

  /// Return the Int64 Value Properties
  ///
  /// This is a vector holding pairs of the integer value name and the number
  /// of value entries per spatial bin.
  ///
  /// This is currently the same for all scalar accumulators
  std::vector<std::pair<std::string,std::size_t>> i64_val_props() const
    noexcept
  { return {{"bin_counts_", n_data_bins_}}; }

  void copy_flt_vals(double *out_vals) const noexcept { }

  void copy_i64_vals(int64_t *out_vals) const noexcept {
    for (std::size_t i = 0; i < bin_counts_.size(); i++){
      out_vals[i] = bin_counts_[i];
    }
  }

private:
  std::size_t n_spatial_bins_;
  std::size_t n_data_bins_;

  // counts_ holds the histogram counts. It holds n_data_bins_*n_spatial_bins_
  // entries. The counts for the ith data bin in the jth spatial bin are stored
  // at index (i + j * n_data_bins_)
  std::vector<int64_t> bin_counts_;

  std::vector<double> data_bin_edges_;
};


#endif /* ACCUMULATORS_H */
