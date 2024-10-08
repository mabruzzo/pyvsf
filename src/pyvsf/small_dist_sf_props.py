from copy import deepcopy
import logging
from typing import Tuple, Sequence, Optional

import numpy as np
from pydantic import BaseModel, conlist, validator, PositiveInt, root_validator

from ._cut_region_iterator import get_root_level_cell_width

from .worker import SFWorker, _PERF_REGION_NAMES, consolidate_partial_vsf_results

from ._kernels import get_kernel, kernel_operates_on_pairs


from ._perf import PerfRegions

# Define some Data Objects


class BoxSelector(BaseModel):
    left_edge: Tuple[float, float, float]
    right_edge: Tuple[float, float, float]
    length_unit: str

    class Config:
        allow_mutation = False

    def apply_selector(self, ds, **kwargs):
        left_edge = ds.arr(self.left_edge, self.length_unit)
        right_edge = ds.arr(self.right_edge, self.length_unit)
        return ds.box(left_edge=left_edge, right_edge=right_edge, **kwargs)

    def get_bbox(self):
        return (np.array(self.left_edge), np.array(self.right_edge), self.length_unit)


_ytfield_type = Tuple[str, str]


def _validate_ytfield(val: _ytfield_type) -> _ytfield_type:
    if (not isinstance(val, tuple)) or len(val) != 2:
        raise ValueError("must be tuple of 2 vals")
    elif not (isinstance(val[0], str) and isinstance(val[1], str)):
        raise ValueError("each item must be a string")
    return val


def _validate_bin_edges(val: Sequence[float]) -> Sequence[float]:
    if len(val) < 2:
        raise ValueError("must have at least 2 entries")
    out = tuple(val)
    if np.any(np.diff(out) <= 0.0):
        raise ValueError("must monotonically increase")
    return out


class StructureFuncProps(BaseModel):
    dist_bin_edges: Sequence[float]
    dist_units: str
    quantity_components: conlist(_ytfield_type, min_items=1, max_items=3)
    quantity_units: str
    cut_regions: conlist(Optional[str], min_items=1)
    max_points: Optional[PositiveInt] = ...
    geometric_selector: Optional[BoxSelector]

    # validators
    _validate_comp = validator("quantity_components", each_item=True, allow_reuse=True)(
        _validate_ytfield
    )
    _validate_dist_bin_edges = validator("dist_bin_edges", allow_reuse=True)(
        _validate_bin_edges
    )

    class Config:
        allow_mutation = False


class SubVolumeDecomposition(BaseModel):
    left_edge: Tuple[float, float, float]
    right_edge: Tuple[float, float, float]
    length_unit: str
    subvols_per_ax: Tuple[PositiveInt, PositiveInt, PositiveInt]
    periodicity: Tuple[bool, bool, bool]
    intrinsic_decomp: bool

    class Config:
        allow_mutation = False

    @root_validator(pre=False)
    def check_edge(cls, values):
        for i in range(3):
            assert values["left_edge"][i] < values["right_edge"][i]
        return values

    @property
    def subvol_widths(self):
        l, r = np.array(self.left_edge), np.array(self.right_edge)
        return (r - l) / np.array(self.subvols_per_ax), self.length_unit

    def valid_subvol_index(self, subvol_index):
        if len(subvol_index) != 3:
            raise ValueError(f"Invalid subvol_index: {subvol_index}")
        itr = [0, 1, 2]
        return all(int(subvol_index[i]) == subvol_index[i] for i in itr) and all(
            0 <= subvol_index[i] < self.subvols_per_ax[i] for i in itr
        )


def _fmt_subvol_index(subvol_index):
    return f"({subvol_index[0]:2d}, {subvol_index[1]:2d}, {subvol_index[2]:2d})"


def decompose_volume(ds, sf_params, subvol_side_len=None, force_subvols_per_ax=None):
    """
    Constructs an instance of SubVolumeDecomposition.

    Paramters
    ---------
    ds
        The dataset object
    sf_params: StructureFuncProps
        structure function parameters.
    subvol_side_len: tuple, Optional
        Optional argument that specifies the length of each axis of the
        subvolume (assuming that its a cube). When specified, this should
        be a 2-tuple where the first element is a positive float specifying the
        length and the second element is a string specifying the units.
        This can't be specified if force_subvols_per_ax is specified.

    Notes
    -----
    If we want to support calculation of structure function as a function of
    position in the future (analagous to a STFT without overlap), it might be
    nice to support the following:
    - Specify an arbitrary subvolume width
    - let SubVolumeDecomposition support a nominal subvolume size for all
      subvolumes other than those adjacent to a boundary. Those adjacent
      subvolumes could have a smaller width (for a non-periodic boundary). If
      we did this, we would presumably want to center the standard sized
      subvolumes on the remainder of the domain.
    """

    # it might be nice to be able to specify the max resolvable length in a
    # subvolume

    kwargs = {}

    if sf_params.geometric_selector is not None:
        left, right, len_u = sf_params.geometric_selector.get_bbox()
        kwargs["left_edge"] = tuple(left)
        kwargs["right_edge"] = tuple(right)
    else:
        len_u = str(ds.domain_left_edge.units)
        kwargs["left_edge"] = tuple(ds.domain_left_edge.v)
        kwargs["right_edge"] = tuple(ds.domain_right_edge.to(len_u).v)
    kwargs["length_unit"] = len_u

    width = ds.arr(
        np.array(kwargs["right_edge"]) - np.array(kwargs["left_edge"]), len_u
    )
    assert (width > 0).all()

    if force_subvols_per_ax is not None:
        assert len(force_subvols_per_ax) == 3
        if any(int(e) != e for e in force_subvols_per_ax):
            raise ValueError("force_subvols_per_ax includes a non-integer")
        force_subvols_per_ax = tuple(int(e) for e in force_subvols_per_ax)
        if any(e <= 0 for e in force_subvols_per_ax):
            raise ValueError("force_subvols_per_ax includes a non-positive integer")

    if force_subvols_per_ax == (1, 1, 1):
        kwargs["subvols_per_ax"] = (1, 1, 1)
    else:
        # retrieve the root-level cell_width in units of 'code_length'
        root_level_cell_width = get_root_level_cell_width(ds)

        # retrieve the max edge of a distance bin
        max_dist_bin_edge = ds.quan(
            np.amax(sf_params.dist_bin_edges), sf_params.dist_units
        )

        # if the edges of our subvolumes were always guaranteed to be aligned
        # with the edges of a root level cell, and there were no round-off
        # error, then the min_subvol_width should exactly be:
        #  > root_level_cell_width.to(sf_params.dist_units) + max_dist_bin_edge
        # If the subvolume widths were any smaller, then we could miss pairs of
        # points at a separation of max_dist_bin_edge
        #
        # Since we aren't taking the care to do that, we will double the
        # contribution from the root_cell_width
        cell_width = root_level_cell_width.to(sf_params.dist_units)
        if (cell_width < 1e-5 * (cell_width + max_dist_bin_edge)).any():
            # this is meant as an indication that there could be a problem
            # (round-off errors might start dropping pairs). The threshold was
            # chosen arbitrarily (the relative size probably needs to be a lot
            # smaller than 1e-5)
            raise RuntimeError("consider adjusting the fudge factor")

        min_subvol_width = max_dist_bin_edge + 2 * cell_width

        max_subvols_per_ax = tuple(
            map(
                lambda x: max(1, int(x)),
                np.floor((width / min_subvol_width).to("dimensionless").v),
            )
        )

        if subvol_side_len is not None:
            subvol_side_len = ds.quan(*subvol_side_len)
            if force_subvols_per_ax is not None:
                raise ValueError(
                    "subvol_side_len and force_subvols_per_ax "
                    "can't both be specified."
                )
            elif subvol_side_len < min_subvol_width.max():
                raise ValueError(
                    f"subvol_side_len, {subvol_side_len}, is too small. It "
                    f"must be at least {min_subvol_width.max()}"
                )
            subvols_per_ax = tuple(
                map(
                    lambda x: max(1, int(x)),
                    np.floor((width / subvol_side_len).to("dimensionless").v),
                )
            )
            if (width != (np.array(subvols_per_ax) * subvol_side_len)).any():
                raise ValueError(
                    "The quotient of the domain with along each axis and "
                    "subvol_side_len must be a positive integer"
                )

            kwargs["subvols_per_ax"] = subvols_per_ax

        elif force_subvols_per_ax is not None:
            for i in range(3):
                if max_subvols_per_ax[i] < force_subvols_per_ax[i]:
                    raise ValueError(
                        "Based on the parameters, the max number of "
                        f"subvols along axis {i} is {max_subvols_per_ax[i]}. "
                        f"The user requested {force_subvols_per_ax[i]}."
                    )
            kwargs["subvols_per_ax"] = force_subvols_per_ax
        else:
            kwargs["subvols_per_ax"] = max_subvols_per_ax

    # TODO fix periodicity handling
    # Note: ds.periodicity doesn't store the right values for EnzoPDatasets
    kwargs["periodicity"] = (False, False, False)
    return SubVolumeDecomposition(intrinsic_decomp=False, **kwargs)


def subvol_index_batch_generator(
    subvol_decomp, n_workers, subvols_per_chunk=None, max_subvols_per_chunk=None
):
    num_x, num_y, num_z = subvol_decomp.subvols_per_ax

    if subvols_per_chunk is None:
        if n_workers == 1:
            chunksize = num_x
        elif n_workers % (num_y * num_z) == 0:
            chunksize = num_x
        else:
            num_subvols = num_x * num_y * num_z
            chunksize, remainder = divmod(num_subvols, 2 * n_workers)
            if remainder != 0:
                chunksize += 1
            chunksize = min(chunksize, num_x)
    else:
        assert subvols_per_chunk <= num_x
        chunksize = subvols_per_chunk
    assert chunksize > 0

    if max_subvols_per_chunk is not None:
        if (subvols_per_chunk is not None) and (
            subvols_per_chunk > max_subvols_per_chunk
        ):
            raise ValueError("subvols_per_chunk can't exceed " "max_subvols_per_chunk")
        elif int(max_subvols_per_chunk) != max_subvols_per_chunk:
            raise ValueError("max_subvols_per_chunk must be an integer")
        elif max_subvols_per_chunk <= 0:
            raise ValueError("max_subvols_per_chunk must be positive")

        chunksize = min(chunksize, max_subvols_per_chunk)

    cur_batch = []

    for z_ind in range(num_z):
        for y_ind in range(num_y):
            for x_ind in range(num_x):
                cur_batch.append((x_ind, y_ind, z_ind))
                if len(cur_batch) == chunksize:
                    yield tuple(cur_batch)
                    cur_batch = []
    if len(cur_batch) > 0:
        yield tuple(cur_batch)


class _PoolCallback:
    def __init__(
        self,
        stat_kw_pairs,
        n_cut_regions,
        subvol_decomp,
        dist_bin_edges,
        autosf_subvolume_callback,
        structure_func_props,
    ):
        # the following are constants:
        self.stat_kw_pairs = stat_kw_pairs
        self.n_cut_regions = n_cut_regions
        self.subvol_decomp = subvol_decomp
        self.dist_bin_edges = dist_bin_edges
        self.autosf_subvolume_user_callback = autosf_subvolume_callback
        self.structure_func_props = structure_func_props
        self.total_count = np.prod(subvol_decomp.subvols_per_ax)

        # the following attributes are updated with each call
        self.tmp_result_arr = np.empty(
            shape=(
                len(stat_kw_pairs),
                n_cut_regions,
                np.prod(subvol_decomp.subvols_per_ax),
            ),
            dtype=object,
        )
        self.total_num_points_arr = np.array([0 for _ in range(n_cut_regions)])

        self.accum_rslt = {}
        for stat_ind, (stat_name, stat_kw) in enumerate(stat_kw_pairs):
            # check if consolidation is commutative
            dset_props = get_kernel(stat_name).get_dset_props(
                dist_bin_edges=dist_bin_edges, kwargs=stat_kw
            )
            commutative_consolidate = all(
                np.issubdtype(dtype, np.integer) for name, dtype, shape in dest_props
            )
            if commutative_consolidate:
                self.accum_rslt[stat_ind] = [{} for _ in range(n_cut_regions)]

        self.cumulative_count = -1
        self.cumulative_perf = PerfRegions(_PERF_REGION_NAMES)

    def __call__(self, batched_result):
        subvols_per_ax = self.subvol_decomp.subvols_per_ax
        autosf_subvolume_callback = self.autosf_subvolume_user_callback

        for item in batched_result:
            subvol_index = item.subvol_index

            subvol_index_1D = subvol_index[0] + subvols_per_ax[0] * (
                subvol_index[1] + subvols_per_ax[1] * subvol_index[2]
            )

            # subvol_available_pts is a lists of the available points from
            # just the subvolume at subvol_index (there is an entry for each
            # cut_region).
            subvol_available_pts = item.main_subvol_available_points

            main_subvol_rslts = item.main_subvol_rslts
            consolidated_rslts = item.consolidated_rslts

            for stat_ind, (stat_name, stat_kw) in enumerate(self.stat_kw_pairs):
                kernel = get_kernel(stat_name)
                for cut_region_i in range(self.n_cut_regions):
                    # for the given subvol_index, stat_index, cut_region_index:
                    # - main_subvol_rslts holds the contributions from just the
                    #   subvolume at subvol_index to the total structure
                    #   function
                    # - consolidated_rslt includes the contribution from
                    #   main_subvol_rslt as well as cross-term contributions
                    #   between points in subvol_index and points in its 13 (or
                    #   at least those that exist) nearest neigboring
                    #   subvolumes on the right side
                    consolidated_rslt = consolidated_rslts.retrieve_result(
                        stat_ind, cut_region_i
                    )

                    if stat_ind in self.accum_rslt:
                        # in the case, consolidation of the statistic is
                        # commutative
                        self.accum_rslt[stat_ind][cut_region_i] = (
                            consolidate_partial_vsf_results(
                                stat_name,
                                self.accum_rslt[stat_ind][cut_region_i],
                                consolidated_rslt,
                                stat_kw=stat_kw,
                                dist_bin_edges=self.dist_bin_edges,
                            )
                        )
                    else:
                        self.tmp_result_arr[stat_ind, cut_region_i, subvol_index_1D] = (
                            consolidated_rslt
                        )

                    if autosf_subvolume_callback is not None:
                        main_subvol_rslt = main_subvol_rslts.retrieve_result(
                            stat_ind, cut_region_i
                        )
                        tmp = deepcopy(main_subvol_rslt)
                        kernel.postprocess_rslt(tmp, kwargs=stat_kw)

                        autosf_subvolume_callback(
                            self.structure_func_props,
                            self.subvol_decomp,
                            subvol_index,
                            stat_ind,
                            cut_region_i,
                            tmp,
                            subvol_available_pts[cut_region_i],
                        )

            # we only update total_num_points_arr once per task rslt
            self.total_num_points_arr[:] += subvol_available_pts

            _str_prefix = f"Driver: {_fmt_subvol_index(subvol_index)} - "
            self.cumulative_count += 1
            self.cumulative_perf = self.cumulative_perf + item.perf_region

            template = (
                (
                    "{_str_prefix} subvol #{cum_count} of {total_count} "
                    + "({n_neighbors:2d} neigbors)\n"
                )
                + "{pad}perf-sec - {perf_summary}\n"
                + "{pad}num points from subvol: {subvol_available_pts}\n"
                + "{pad}total num points: {total_num_points_arr}"
            )

            print(
                template.format(
                    _str_prefix=_str_prefix,
                    pad="    ",
                    cum_count=self.cumulative_count,
                    total_count=self.total_count,
                    n_neighbors=item.num_neighboring_subvols,
                    perf_summary=item.perf_region.summarize_timing_sec(),
                    subvol_available_pts=subvol_available_pts,
                    total_num_points_arr=self.total_num_points_arr,
                )
            )

            item.main_subvol_rslts.purge()
            item.consolidated_rslts.purge()


def _prep_pool(pool=None):
    if pool is None:

        class Pool:
            def map(self, func, iterable, callback=None):
                tmp = map(func, iterable)
                for elem in tmp:
                    if callback is not None:
                        callback(elem)
                    yield elem

        pool = Pool()
        n_workers = 1
    else:
        n_workers = pool.size
    return pool, n_workers


def _consolidate_rslts(stat_kw_pairs, post_proc_callback, dist_bin_edges):
    prop_l = []
    for stat_ind, (stat_name, stat_kw) in enumerate(stat_kw_pairs):
        if stat_ind in post_proc_callback.accum_rslt:
            # the results for this stat are already consolidated
            prop_l.append(post_proc_callback.accum_rslt[stat_ind])
        else:
            tmp = []
            for sublist in post_proc_callback.tmp_result_arr[stat_ind]:
                tmp.append(
                    consolidate_partial_vsf_results(
                        stat_name, *sublist, dist_bin_edges=dist_bin_edges
                    )
                )
            prop_l.append(tmp)

        kernel = get_kernel(stat_name)
        for elem in prop_l[-1]:
            kernel.postprocess_rslt(elem, kwargs=stat_kw)
    return prop_l


_dflt_vel_components = (
    ("gas", "velocity_x"),
    ("gas", "velocity_y"),
    ("gas", "velocity_z"),
)


def small_dist_sf_props(
    ds_initializer,
    dist_bin_edges,
    cut_regions=[None],
    pos_units=None,
    quantity_units=None,
    component_fields=_dflt_vel_components,
    geometric_selector=None,
    statistic="variance",
    kwargs={},
    max_points=None,
    rand_seed=None,
    subvol_side_len=None,
    force_subvols_per_ax=None,
    eager_loading=False,
    max_subvols_per_chunk=None,
    pool=None,
    autosf_subvolume_callback=None,
):
    """
    Computes the structure function.

    This function includes optimizations that excel the best when
    `np.amax(dist_bin_edges)` is significantly smaller than the domain's width.
    This function avoids looking at a lot of pairs that are too far apart to
    matter

    Suppose:
    - `d` is `np.amax(dist_bin_edges)` times some small fudge factor
       (e.g. ~1.001) in `pos_units`
    - `L`, `W`, `H`, are dimensions of the domain (in `pos_units`)
    - `n` is the number of points per 1 `pos_unit`.
    The brute-force approach that considers every pair considers:
        `0.5*(L*W*H)**2 * n**6` unique pairs
    The function instead considers roughly (13.5 is slightly too large):
        `13.5 * (L/d) * (W/d) * (H/d) * d**6 * n**6` pairs
    This means that the brute force approach considers a factor of
    `L*W*H/(26 * d^3)` extra pairs

    Parameters
    ----------
    ds_initializer
        The callable that initializes a yt-dataset
    dist_bin_edges: 1D np.ndarray
        Optionally specifies the distance bin edges. A distance `dx` that falls
        in the `i`th bin satisfies the following inequality:
        `dist_bin_edges[i] <= dx < dist_bin_edges[i+1]`
    cut_regions: tuple of strings
        `cut_regions` is list of cut_strings combinations. Examples
        include `"obj['temperature'] > 1e6"`,
        `'obj["velocity_magnitude"].in_units("km/s") > 1'`, and `None`. `None`
        includes all values. A minor optimization can be made if `None` is
        passed as the last tuple entry in subvolumes where another cut_region
        also includes all of the entries in the subvolume.
    pos_units, quantity_units: string, Optional
        Optionally specifies the position and quantity units.
    component_fields: list of fields
        List of 1 to 3 `yt` fields that are used to represent the individual
        components of the quntity for which the structure function properties
        are computed
    geometric_selector: BoxSelector, optional
        Optional specification of a subregion to compute the structure function
        within.
    maxpoints: int, optional
        The maximum number of points to consider in the calculation. When
        unspecified, there is no limit to the maximum number of points.
    rand_seed: int, optional
        Optional argument used to seed the pseudorandom permutation used to
        select points when the number of points exceed `maxpoints`.
    subvol_side_len: tuple, Optional
        Optional argument that specifies the length of each axis of the
        subvolume (assuming that its a cube). When specified, this should
        be a 2-tuple where the first element is a positive float specifying the
        length and the second element is a string specifying the units.
        This can't be specified if force_subvols_per_ax is specified.
    eager_loading: bool, optional
        When True, this tries to load simulation data much more eagerly
        (aggregating reads). While this requires additional RAM, this tries to
        ease pressure on shared file systems.
    max_subvols_per_chunk: int, optional
        The subvolumes are passed to the pool in chunks. This is used to
        optionally specify the maximum number of subvols that are included in a
        chunk.
    pool: `multiprocessing.pool.Pool`-like object, optional
        When specified, this should have a `map` method with a similar
        interface to `multiprocessing.pool.Pool`'s `map method
        and an iterable.
    autosf_subvolume_callback: callable, Optional
        An optional callable that can process the auto-structure function
        properties computed for individual subvolumes (for example, this could
        be used to save such quantities to disk). The callable should expect
        the following arugments
        - an instance of `StructureFuncProps`. This should not be mutated.
        - an instance of `SubVolumeDecomposition` (specifying how the domain is
          broken up). This should not be mutated.
        - the subvolume index (a tuple of 3 integers),
        - stat_index, the index corresponding to the statitic being computed.
          (If you're only computing a single statistic, this will always be 0)
        - the index corresponding to the cut_region
        - the structure function properties computed within the subvolume
        - the number of points in that subvolume that are available to be used
          to compute the structure function properties.

    Returns
    -------
    prop_l: list of dicts
        The properties dictionary for the entire domain (for each cut_region)
    num_points_used_arr: np.ndarray
        The total number of points that were used to compute prop_l (for each
        cut region)
    total_avail_points_arr: np.ndarray
        The total number of points that are available to be used to compute
        structure function properties (for each cut region)
    subvol_decomp: `SubVolumeDecomposition`
        Specifies how the domain has been decomposed into subvolumes
    sf_params: StructureFuncProps
        Summarizes the structure function calculation properties
    """

    if not callable(ds_initializer):
        assert pool is None
        _ds = ds_initializer
        ds_initializer = lambda: _ds

    pool, n_workers = _prep_pool(pool)

    assert len(cut_regions) > 0
    dist_bin_edges = np.asarray(dist_bin_edges, dtype=np.float64)
    if dist_bin_edges.ndim != 1:
        raise ValueError("dist_bin_edges must be a 1D np.ndarray")
    elif dist_bin_edges.size <= 1:
        raise ValueError("dist_bin_edges must have 2 or more elements")
    elif (dist_bin_edges[1:] <= dist_bin_edges[:-1]).any():
        raise ValueError("dist_bin_edges must have monotonically increasing elements")

    if (max_points is None) != (rand_seed is None):
        raise ValueError(
            "max_points and rand_seed must both be " "specified or unspecified"
        )
    if max_points is not None:
        assert int(max_points) == max_points
        max_points = int(maxpoints)
        assert int(rand_seed) == rand_seed
        rand_seed = int(rand_seed)

        # to support this in the future, I think we need to adopt the
        # following procedure (for each cut_region)
        # 1. Root dispatches tasks where every subvolume counts up the number
        #    of valid points and send back to ro
        # 2. Root builds an array which lists the number of points per
        #    subvolume, availpts_per_subvol
        # 3. Root (it doesn't actually have to happen on root). Then randomly
        #    determines how many points come from each subvolume. Below is
        #    pseudo-code to sketch an inefficient way to do this:
        #      >>> gen = np.random.default_rng(seed = rand_seed - 1)
        #      >>> remaining = np.copy(availpts_per_subvol)
        #      >>> drawn = np.zeros_like(remaining)
        #      >>> for i in range(max_points):
        #      >>>     choice = gen.choice(remaining.size,
        #      ...                         p = remaining/remaining.sum())
        #      >>>     drawn[choice] += 1
        #      >>>     drawn[choice] -= 1
        #      >>> assert out.sum() == max_points
        #      >>> assert (remaining >= 0).all()
        # 4. Then, to identify the points for a subvolume, with index
        #    `sv_index`, use the following pseudo-code:
        #     >>> sv_ind1D = # 1D representation for sv_index
        #     >>> gen = np.random.default_rng(seed = rand_seed + sv_ind1D)
        #     >>> ipoints = gen.choice(availpts_per_subvol[sv_ind1D],
        #     ...                      size = drawn[choice], replace = False)

        raise NotImplementedError(
            "Support is not currently provided for randomly drawing a subset "
            "of points"
        )

    # some of the argument checking is automatically performed by validation in
    # structure_func_props
    structure_func_props = StructureFuncProps(
        dist_bin_edges=list(dist_bin_edges),
        dist_units=pos_units,
        quantity_components=component_fields,
        quantity_units=quantity_units,
        cut_regions=cut_regions,
        max_points=max_points,
        geometric_selector=geometric_selector,
    )

    subvol_decomp = decompose_volume(
        ds_initializer(),
        structure_func_props,
        subvol_side_len=subvol_side_len,
        force_subvols_per_ax=force_subvols_per_ax,
    )

    logging.info(f"Number of subvolumes per axis: {subvol_decomp.subvols_per_ax}")

    if isinstance(statistic, str):
        if not isinstance(kwargs, dict):
            raise ValueError("kwargs must be a dict when statistic is a string")
        stat_kw_pairs = [(statistic, kwargs)]
        single_statistic = True
    elif len(statistic) == 0:
        raise ValueError("statistic can't be an empty sequence")
    elif not all(isinstance(e, str) for e in statistic):
        raise TypeError("statistic must be a string or a sequence of strings")
    elif isinstance(kwargs, dict) or not all(isinstance(e, dict) for e in kwargs):
        raise ValueError(
            "When statistic is a sequence of strings, kwargs "
            "must be a sequence of dicts."
        )
    elif len(statistic) != len(kwargs):
        raise ValueError(
            "When statistic is a sequence of strings, kwargs "
            "must be a sequence of as many dicts."
        )
    elif np.unique(statistic).size != len(statistic):
        raise ValueError(
            "When statistic is a sequence of strings, none of "
            "the strings are allowed to be duplicates."
        )
    else:
        stat_kw_pairs = list(zip(statistic, kwargs))
        single_statistic = False

    del statistic, kwargs  # deleted for debugging purposes

    worker = SFWorker(
        ds_initializer,
        subvol_decomp,
        sf_param=structure_func_props,
        stat_kw_pairs=stat_kw_pairs,
        eager_loading=eager_loading,
    )

    iterable = subvol_index_batch_generator(
        subvol_decomp, n_workers=n_workers, max_subvols_per_chunk=max_subvols_per_chunk
    )

    post_proc_callback = _PoolCallback(
        stat_kw_pairs,
        n_cut_regions=len(cut_regions),
        subvol_decomp=subvol_decomp,
        dist_bin_edges=dist_bin_edges,
        autosf_subvolume_callback=autosf_subvolume_callback,
        structure_func_props=structure_func_props,
    )

    for batched_result in pool.map(worker, iterable, callback=post_proc_callback):
        continue  # simply consume the iterator

    print(
        "Cumulative subvol-processing perf-sec -\n    "
        + post_proc_callback.cumulative_perf.summarize_timing_sec()
    )

    # now, let's consolidate the results together
    prop_l = _consolidate_rslts(stat_kw_pairs, post_proc_callback, dist_bin_edges)

    if single_statistic:
        prop_l = prop_l[0]
    total_num_points_used_arr = np.array(post_proc_callback.total_num_points_arr)
    total_avail_points_arr = np.array(post_proc_callback.total_num_points_arr)
    return (
        prop_l,
        total_num_points_used_arr,
        total_avail_points_arr,
        subvol_decomp,
        structure_func_props,
    )


# --------------------------

from .grid_scale.worker import WorkerStructuredGrid
from .grid_scale._utils import _top_level_grid_indices


def decompose_volume_intrinsic(ds):
    """
    Constructs an instance of SubVolumeDecomposition (that matches the
    intrinsic subvolumes)

    Paramters
    ---------
    ds
        The dataset object
    """

    kwargs = {}
    len_u = str(ds.domain_left_edge.units)
    kwargs["left_edge"] = tuple(ds.domain_left_edge.v)
    kwargs["right_edge"] = tuple(ds.domain_right_edge.to(len_u).v)
    kwargs["length_unit"] = str(ds.domain_left_edge.units)

    width = ds.arr(
        np.array(kwargs["right_edge"]) - np.array(kwargs["left_edge"]), len_u
    )
    assert (width > 0).all()

    kwargs["subvols_per_ax"] = _top_level_grid_indices(ds).shape
    # TODO fix periodicity handling
    # Note: ds.periodicity doesn't store the right values for EnzoPDatasets
    kwargs["periodicity"] = (False, False, False)
    return SubVolumeDecomposition(intrinsic_decomp=True, **kwargs)


def grid_scale_vel_diffs(
    ds_initializer,
    cut_regions=[None],
    component_fields=_dflt_vel_components,
    max_subvols_per_chunk=3,
    pool=None,
):
    """
    Computes velocity differences on the grid scale

    This bears a lot of similarities to small_dist_sf_props. Maybe we can
    consolidate?

    Parameters
    ----------

    pool: `multiprocessing.pool.Pool`-like object, optional
        When specified, this should have a `map` method with a similar
        interface to `multiprocessing.pool.Pool`'s `map method
        and an iterable.
    """

    if not callable(ds_initializer):
        assert pool is None
        _ds = ds_initializer
        ds_initializer = lambda: _ds

    pool, n_workers = _prep_pool(pool)

    # some of the argument checking is automatically performed by validation in
    # structure_func_props
    structure_func_props = StructureFuncProps(
        dist_bin_edges=[0, 1],
        dist_units="code_length",
        quantity_components=component_fields,
        quantity_units="code_velocity",
        cut_regions=cut_regions,
        max_points=None,
        geometric_selector=None,
    )

    subvol_decomp = decompose_volume_intrinsic(ds_initializer())

    logging.info(f"Number of subvolumes per axis: {subvol_decomp.subvols_per_ax}")

    # setup the stat-kw pairs
    aligned_edges = np.array(
        [-np.inf] + np.linspace(-3, 3, num=121).tolist() + [np.inf]
    )
    transverse_edges = np.array(np.linspace(0, 3, num=121).tolist() + [np.inf])
    stat_kw_pairs = [
        (
            "grid_vdiff_histogram",
            {
                "aligned_vdiff_edges": aligned_edges,
                "transverse_vdiff_edges": transverse_edges,
                "mag_vdiff_edges": transverse_edges.copy(),
            },
        )
    ]
    single_statistic = True

    worker = WorkerStructuredGrid(
        ds_initializer,
        subvol_decomp,
        sf_param=structure_func_props,
        stat_kw_pairs=stat_kw_pairs,
    )

    iterable = subvol_index_batch_generator(
        subvol_decomp, n_workers=n_workers, max_subvols_per_chunk=max_subvols_per_chunk
    )

    post_proc_callback = _PoolCallback(
        stat_kw_pairs,
        n_cut_regions=len(cut_regions),
        subvol_decomp=subvol_decomp,
        dist_bin_edges=np.array(structure_func_props.dist_bin_edges),
        autosf_subvolume_callback=None,
        structure_func_props=structure_func_props,
    )

    for batched_result in pool.map(worker, iterable, callback=post_proc_callback):
        continue  # simply consume the iterator

    print(
        "Cumulative subvol-processing perf-sec -\n    "
        + post_proc_callback.cumulative_perf.summarize_timing_sec()
    )

    # now, let's consolidate the results together
    prop_l = _consolidate_rslts(
        stat_kw_pairs, post_proc_callback, np.array(structure_func_props.dist_bin_edges)
    )

    if single_statistic:
        prop_l = prop_l[0]
    total_num_points_used_arr = np.array(post_proc_callback.total_num_points_arr)
    total_avail_points_arr = np.array(post_proc_callback.total_num_points_arr)
    return (
        prop_l,
        total_num_points_used_arr,
        stat_kw_pairs[0][1],
        structure_func_props,
    )
