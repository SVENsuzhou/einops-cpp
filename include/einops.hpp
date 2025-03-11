#pragma once

#include <backends.hpp>
#include <parsing.hpp>

namespace einops {
namespace implementation {

const auto _reductions = Reductions({ "min", "max", "sum", "mean", "prod" });
const auto _unknown_axis_length = Axis(-999999);
const auto _expected_axis_length = Axis(-99999);
const auto _ellipsis_not_in_parenthesis = Axes({ -999 });

inline auto _product(std::vector<int64_t> const& sequence) -> int64_t
{
	int64_t result = 1;
	for (auto element : sequence)
		result *= element;
	return result;
}

static LRUCache<Hash, TransformRecipe> _transformRecipeCache (256);

static auto _prepare_transformation_recipe(Pattern const& pattern, Reduction const& operation, AxesLengths const& axes_names, int64_t ndim) -> TransformRecipe
{
	auto hash = HashBuilder()(pattern, operation, print(axes_names), print(ndim));
	if (_transformRecipeCache.exists(hash))
		return _transformRecipeCache.get(hash);

	auto&& [left_str, rght_str] = divide(pattern, "->");

	auto left = ParsedExpression(left_str);
	auto rght = ParsedExpression(rght_str);

	if (!left.has_ellipsis && rght.has_ellipsis)
		throw Exception(::format("Ellipsis found in right side, but not left side of a pattern {}", pattern));

	if (left.has_ellipsis && left.has_ellipsis_parenthesized)
		throw Exception(::format("Ellipsis is parenthesis in the left side is not allowed: {}", pattern));

	if (operation == "rearrange")
	{
		if (left.has_non_unitary_anonymous_axes || rght.has_non_unitary_anonymous_axes)
			throw Exception("Non-unitary anonymous axes are not supported in rearrange (exception is length 1)");

		auto diff = symmetric_difference(left.identifiers, rght.identifiers);
		
		if (diff.size() > 0)
			throw Exception(format("Identifiers only on one side of expression (should be on both): {}", print(diff)));
	}
	else
	if (operation == "repeat")
	{
		auto diff = difference(left.identifiers, rght.identifiers);

		if (diff.size() > 0)
			throw Exception(format("Unexpected identifiers on the left side of repeat: {}", print(diff)));

		Identifiers left_ident;
		for (auto&& ax : rght.identifiers)
			if (ax.index() != 1)
				left_ident.insert(ax);

		Identifiers right_ident = left.identifiers;
		for (auto&& [name, index] : axes_names)
			right_ident.insert(name);

		auto axes_without_size = difference(left_ident, right_ident);

		if (axes_without_size.size() > 0)
			throw Exception(format("Specify sizes for new axes in repeat: {}", print(axes_without_size)));
	}
	else
	if (contains(_reductions, operation)) // TODO: callable using lambda functor !
	{
		auto diff = difference(rght.identifiers, left.identifiers);

		if (diff.size() > 0)
			throw Exception(format("Unexpected identifiers on the right side of reduce {}: {}", operation, print(diff)));
	}
	else
		throw Exception(format("Unknown reduction {}. Expect one of {}.", operation, print(_reductions)));

	Composition left_composition;
	Composition rght_composition;

	if (left.has_ellipsis)
	{
		auto n_other_dims = static_cast<int>(left.composition.size() - 1);
		if (ndim < n_other_dims)
			throw Exception(format("Wrong shape: expected >={} dims. Received {}-dim tensor.", n_other_dims, ndim));

		auto ellipsis_ndim = ndim - n_other_dims;
		std::vector<std::string> ell_axes;
		for (auto i : iters::range(ellipsis_ndim))
			ell_axes.push_back(_ellipsis + std::to_string(i));

		for (auto&& composite_axis : left.composition)
		{
			if (composite_axis.index() == 1)
			{
				for (auto&& axis : ell_axes)
					left_composition.push_back({ axis });
			}
			else
				left_composition.push_back(std::get<0>(composite_axis));
		}

		for (auto&& composite_axis : rght.composition)
		{
			if (composite_axis.index() == 1)
			{
				for (auto&& axis : ell_axes)
					rght_composition.push_back({ axis });
			}
			else
			{
				std::vector<std::string> group;
				for (auto&& axis : std::get<0>(composite_axis))
				{
					if (axis == _ellipsis)
						group.insert(group.begin(), ell_axes.begin(), ell_axes.end());
					else
						group.push_back(axis);
				}
				rght_composition.push_back(group);
			}
		}

		left.identifiers.insert(ell_axes.begin(), ell_axes.end());
		left.identifiers.erase(_ellipsis);
		
		if (rght.has_ellipsis)
		{
			rght.identifiers.insert(ell_axes.begin(), ell_axes.end());
			rght.identifiers.erase(_ellipsis);
		}
	}
	else
	{
		if (ndim != left.composition.size())
			throw Exception(format("Wrong shape: expected {} dims. Received {}-dim tensor.", left.composition.size(), ndim));

		left_composition = left.composition;
		rght_composition = rght.composition;
	}

	IdentifiersMap axis_name2known_length;
	auto insert_in_axis_name2known_length = [&](Identifier const& axis_name)
	{
		if (axis_name.index() == 1) 
			axis_name2known_length[axis_name] = std::get<1>(axis_name).to_integer();
		else
			axis_name2known_length[axis_name] = _unknown_axis_length;
	};

	for (auto&& composite_axis : left_composition)
	{
		if (composite_axis.index() == 0)
		{
			for (auto&& axis_name : std::get<0>(composite_axis))
				insert_in_axis_name2known_length(axis_name);
		}
		else
		{
			insert_in_axis_name2known_length(std::get<1>(composite_axis));
		}
	}

	for (auto&& axis_name : rght.identifiers)
	{
		if (!axis_name2known_length.count(axis_name))
		{
			insert_in_axis_name2known_length(axis_name);
		}
	}

	IdentifiersMap axis_name2position;
	for (auto&& [position, name] : iters::enumerate(axis_name2known_length))
		axis_name2position[name.first] = position;

	for (auto&& [elementary_axis, _] : axes_names)
	{
		if (!ParsedExpression::check_axis_name(elementary_axis))
			throw Exception(format("Invalid name for an axis {}", print(elementary_axis)));

		if (!axis_name2known_length.count(elementary_axis))
			throw Exception(format("Axis {} is not used in transform", print(elementary_axis)));

		axis_name2known_length[elementary_axis] = _expected_axis_length;
	}

	InputCompositeAxes input_axes_known_unknown;

	for (auto&& [i, composite_axis] : iters::enumerate(left_composition))
	{
		std::set<std::string> known;
		std::set<std::string> unknown;

		if (composite_axis.index() == 0)
		{
			auto&& comp_axis = std::get<0>(composite_axis);

			for (auto&& axis : comp_axis)
				if (axis_name2known_length[axis] != _unknown_axis_length)
					known.insert(axis);

			for (auto&& axis : comp_axis)
				if (axis_name2known_length[axis] == _unknown_axis_length)
					unknown.insert(axis);

			if (unknown.size() > 1)
				throw Exception(format("Could not infer sizes for {}", print(unknown)));

			assert(unknown.size() + known.size() == comp_axis.size());
		}
		else
		{
			auto&& axis = std::get<1>(composite_axis);

			if (axis_name2known_length[axis] == _unknown_axis_length)
				unknown.insert(axis);
			else
				known.insert(axis);
		}

		Axes input_axes_known_vec;
		Axes input_axes_unknown_vec;

		for (auto&& know_axis : known)
			input_axes_known_vec.push_back(axis_name2position[know_axis]);

		for (auto&& unknow_axis : unknown)
			input_axes_unknown_vec.push_back(axis_name2position[unknow_axis]);

		input_axes_known_unknown.push_back
		({
			input_axes_known_vec,
			input_axes_unknown_vec
		});
	}

	OutputCompositeAxes result_axes_grouping;
	for (auto&& composite_axis : rght_composition)
	{
		if (composite_axis.index() == 0)
		{
			auto&& comp_axis = std::get<0>(composite_axis);

			Axes axes_grouping;
			for (auto&& axis : comp_axis)
				axes_grouping.push_back(axis_name2position[axis]);

			result_axes_grouping.push_back(axes_grouping);
		}
		else
		{
			auto&& axis = std::get<1>(composite_axis);

			Axes axes_grouping;
			axes_grouping.push_back(axis_name2position[axis]);

			result_axes_grouping.push_back(axes_grouping);
		}
	}

	auto ordered_axis_left = list(iters::chain(left_composition));
	auto ordered_axis_rght = list(iters::chain(rght_composition));

	std::vector<Identifier> reduced_axes;
	for (auto&& axis : ordered_axis_left)
	{
		if (!rght.identifiers.count(axis))
			reduced_axes.push_back(axis);
	}

	std::vector<Identifier> order_after_transposition;
	for (auto&& axis : ordered_axis_rght)
	{
		if (left.identifiers.count(axis))
			order_after_transposition.push_back(axis);
	}

	order_after_transposition.insert(
	order_after_transposition.end(),
				 reduced_axes.begin(), 
				 reduced_axes.end());

	Axes axes_permutation;
	for (auto&& axis : order_after_transposition)
		axes_permutation.push_back(index(ordered_axis_left, print(axis)));

	AxesMap added_axes;
	for (auto&& [i, axis_name] : iters::enumerate(ordered_axis_rght))
		if (!left.identifiers.count(axis_name))
			added_axes[i] = axis_name2position[axis_name];

	Axis first_reduced_axis = order_after_transposition.size() - reduced_axes.size();

	auto elementary_axes_lengths = values(axis_name2known_length);

	IdentifiersMap axis_name2elementary_axis;
	for (auto&& [axis, _] : axes_names)
		axis_name2elementary_axis[axis] = axis_name2position[axis];

	auto recipe = TransformRecipe
	{
		elementary_axes_lengths,
		axis_name2elementary_axis,
		input_axes_known_unknown,
		axes_permutation,
		first_reduced_axis,
		added_axes,
		result_axes_grouping,
		hash // hold is own hash
	};

	_transformRecipeCache.put(hash, recipe);

	return recipe;
}

template <typename AxesLengths>
inline auto _reconstruct_from_shape_uncached(TransformRecipe const& self, Shape const& shape, AxesLengths const& axes_dims) -> CookedRecipe
{
	auto need_init_reshape = false;

	Axes axes_lengths = self.elementary_axes_lengths;
	for (auto&& [axis, dim] : axes_dims)
		axes_lengths[self.axis_name2elementary_axis.at(axis)] = dim;

	for (auto&& [input_axis, known_unknown_axes] : iters::enumerate(self.input_composition_known_unknown))
	{
		auto&& [known_axes, unknown_axes] = known_unknown_axes;
		auto length = shape[input_axis];
		if (known_axes.size() == 0 && unknown_axes.size() == 1)
		{
			axes_lengths[unknown_axes[0]] = length;
			continue;
		}

		int64_t known_product = 1;
		for (auto&& axis : known_axes)
			known_product *= axes_lengths[axis];

		if (unknown_axes.size() == 0)
		{
			if (/* TODO */length != known_product)
				throw Exception(format("Shape mismatch, {} != {}", length, known_product));
		}
		else
		{
			if (/* TODO */length % known_product != 0)
				throw Exception(format("Shape mismatch, can't divide axis of length {} in chunks of {}", length, known_product));

			auto unknown_axis = unknown_axes[0];
			auto inferred_length = int64_t(std::floor(length / known_product));
			axes_lengths[unknown_axis] = inferred_length;
		}

		if (known_axes.size() + unknown_axes.size() != 1)
			need_init_reshape = true;
	}

	OptionalAxes init_shapes = std::nullopt;
	if (need_init_reshape)
		init_shapes = Axes{ axes_lengths.begin(), axes_lengths.begin() + self.axes_permutation.size() };

	auto need_final_reshape = false;

	Axes final_shapes;
	for (auto&& grouping : self.output_composite_axes)
	{
		Axes lengths;
		for (auto&& elementary_axis : grouping)
			lengths.push_back(axes_lengths[elementary_axis]);
		
		final_shapes.push_back(_product(lengths));

		if (lengths.size() != 1)
			need_final_reshape = true;
	}

	AxesMap added_axes;
	for (auto&& [pos, pos_in_elementary] : self.added_axes)
		added_axes[pos] = axes_lengths[pos_in_elementary];

	Axes reduced_axes = iters::range<Axis>(self.first_reduced_axis, self.axes_permutation.size()).vec();
	
	Axis n_axes_after_adding_axes = added_axes.size() + self.axes_permutation.size();

	OptionalAxes axes_reordering = self.axes_permutation;
	if (compare<Axis>(self.axes_permutation, iters::range<Axis>(self.axes_permutation.size()).vec()))
		axes_reordering = std::nullopt;

	OptionalAxes _final_shapes = std::nullopt;
	if (need_final_reshape)
		_final_shapes = final_shapes;

	return { init_shapes, axes_reordering, reduced_axes, added_axes, _final_shapes, n_axes_after_adding_axes };
}

static LRUCache<Hash, CookedRecipe> _reconstructFromShapeCache (1024);

template <typename AxesLengths>
inline auto _reconstruct_from_shape(TransformRecipe const& self, Shape const& shape, AxesLengths const& axes_dims) -> CookedRecipe
{
	auto hash = HashBuilder()(self.hash, print(shape), print(axes_dims));
	if (_reconstructFromShapeCache.exists(hash))
		return _reconstructFromShapeCache.get(hash);

	auto recipe = _reconstruct_from_shape_uncached(self, shape, axes_dims);

	_reconstructFromShapeCache.put(hash, recipe);

	return recipe;
}

inline auto _prepare_recipes_for_all_dims(Pattern const& pattern, Reduction const& operation, AxesLengths const& axes_names) -> std::map<int64_t, TransformRecipe>
{
	auto&& [left_str, _] = divide(pattern, "->");
	auto&& left = ParsedExpression(left_str);
	auto&& dims = Axes{ (Axis)left.composition.size() };
	if (left.has_ellipsis)
	{
		dims.clear();
		for (auto ellipsis_dims : iters::range(8))
			dims.push_back(left.composition.size() - 1 + ellipsis_dims);
	}
	std::map<int64_t, TransformRecipe> output;
	for (auto&& ndim : dims) 
		output[ndim] = _prepare_transformation_recipe(pattern, operation, axes_names, ndim);
	return output;
}

template <typename Tensor, typename Backend>
inline Tensor _reduce_axes(Tensor const& tensor, Reduction const& reduction_type, Axes const& reduced_axes, Backend& backend)
{
	assert(contains(_reductions, reduction_type));
	if (reduction_type == "mean")
		if (!backend.is_float_type(tensor))
			throw Exception("reduce_mean is not available for non-floating tensors");
	return backend.reduce(tensor, reduction_type, reduced_axes);
}

template <typename Tensor, typename Backend, typename AxesLengths>
inline Tensor _apply_recipe(Backend& backend, TransformRecipe const& recipe, Tensor tensor, Reduction const& reduction_type, AxesLengths const& axes_lengths)
{
	auto&& [init_shapes, axes_reordering, reduced_axes, added_axes, final_shapes, n_axes_w_added] 
		 = _reconstruct_from_shape(recipe, backend.shape(tensor), axes_lengths);

	if (init_shapes.has_value())
		tensor = backend.reshape(tensor, init_shapes.value());
	if (axes_reordering.has_value())
		tensor = backend.transpose(tensor, axes_reordering.value());
	if (reduced_axes.size() > 0)
		tensor = _reduce_axes(tensor, reduction_type, reduced_axes, backend);
	if (added_axes.size() > 0)
		tensor = backend.add_axes(tensor, n_axes_w_added, added_axes);
	if (final_shapes.has_value())
		tensor = backend.reshape(tensor, final_shapes.value());

	return tensor;
}

template <typename T>
inline auto _validate_einsum_axis_name(T const& value) -> std::string
{
	if (value.index() == 0)
	{
		auto axis_names = std::get<0>(value);

		if (axis_names.size() == 0)
			throw Exception("Singleton () axes are not yet supported in einsum.");

		if (axis_names.size() > 1)
			throw Exception("Shape rearrangement is not yet supported in einsum.");

		auto axis_name = axis_names[0];
		
		if (axis_name.empty())
			throw Exception("Encountered empty axis name in einsum.");

		return axis_name;
	}
	else
	{
		auto axis_name = std::get<1>(value);

		if (axis_name.empty())
			throw Exception("Encountered empty axis name in einsum.");

		return axis_name;
	}
}

static LRUCache<Hash, std::string> _compactifyPatternForEinsumCache (256);

inline std::string _compactify_pattern_for_einsum(std::string const& pattern)
{
	auto hash = HashBuilder()(pattern);
	if (_compactifyPatternForEinsumCache.exists(hash))
		return _compactifyPatternForEinsumCache.get(hash);

	if (!contains(pattern, "->"))
		throw Exception("Einsum pattern must contain '->'.");

	auto [lefts_str, right_str] = divide(pattern, "->");

	auto lefts_strs = splits(lefts_str, ",");
	std::vector<ParsedExpression> lefts;
	for (auto&& left : lefts_strs)
		lefts.push_back(ParsedExpression(left, true, true));

	auto right = ParsedExpression(right_str, true);

	std::string output_axis_names = ascii_letters;
	int i = 0;
	std::map<Identifier, std::string> axis_name_mapping;

	std::vector<std::string> left_patterns;
	for (auto&& left : lefts)
	{
		std::string left_pattern = "";
		for (auto&& raw_axis_name : left.composition)
		{
			auto&& axis_name = _validate_einsum_axis_name(raw_axis_name);

			if (axis_name == _ellipsis)
			{
				left_pattern += "...";
				continue;
			}
			
			if (!axis_name_mapping.count(axis_name))
			{
				if (i >= output_axis_names.length())
					throw Exception("Too many axes in einsum.");

				axis_name_mapping[axis_name] = output_axis_names[i];
				i += 1;
			}

			left_pattern += axis_name_mapping[axis_name];
		}
		left_patterns.push_back(left_pattern);
	}

	auto compact_pattern = join(left_patterns, ",") + "->";

	for (auto&& raw_axis_name : right.composition)
	{
		auto&& axis_name = _validate_einsum_axis_name(raw_axis_name);

		if (axis_name == _ellipsis)
		{
			compact_pattern += "...";
			continue;
		}
	
		if (!axis_name_mapping.count(axis_name))
			throw Exception(format("Unknown axis {} on right side of einsum {}.", axis_name, pattern));

		compact_pattern += axis_name_mapping[axis_name];
	}

	_compactifyPatternForEinsumCache.put(hash, compact_pattern);

	return compact_pattern;
}

} // namespace implementation

/// @brief Provides combination of reordering and reduction using reader-friendly notation.
/// @param tensor tensor of any supported library (only libtorch in this version)
/// list of tensors is also accepted, those should be of the same type and shape
/// @param pattern string, rearrangement pattern
/// @param reduction one of available reductions ('min', 'max', 'sum', 'mean', 'prod'), case-sensitive
/// @param axes_lengths any additional specifications for dimensions
/// @return tensor of the same type as input.
template <typename Tensor, typename... Args>
auto reduce(Tensor const& tensors, std::string const& pattern, std::string const& reduction, Args... axes_lengths)
{
	using namespace implementation;

	auto&& [backend, tensor] = backends::get_backend(tensors);
	auto&& shape = backend.shape(tensor);

	AxesLengths hashable_axes_lengths;

	try
	{
		if constexpr (sizeof...(axes_lengths) > 0 && are_all_same<AxesLengthsMap, Args...>)
		{
			hashable_axes_lengths = from_map(axes_lengths...);
			auto recipe = _prepare_transformation_recipe(pattern, reduction, hashable_axes_lengths, shape.size());
			return _apply_recipe(backend, recipe, tensor, reduction, hashable_axes_lengths);
		}
		else
		if constexpr (sizeof...(axes_lengths) > 0)
		{
			hashable_axes_lengths = to_vector(std::tuple<Args...>(axes_lengths...));
			auto recipe = _prepare_transformation_recipe(pattern, reduction, hashable_axes_lengths, shape.size());
			return _apply_recipe(backend, recipe, tensor, reduction, hashable_axes_lengths);
		}
		else
		{
			auto recipe = _prepare_transformation_recipe(pattern, reduction, AxesLengths(), shape.size());
			return _apply_recipe(backend, recipe, tensor, reduction, AxesLengths());
		}
	}
	catch (Exception const& e)
	{
		auto message  = ::format("\n\n Error while processing {}-reduction pattern \"{}\".", reduction, pattern);
			 message += ::format("\n Input tensor shape: {}. ", print(shape));
			 message += ::format("Additional info: {}.", print(hashable_axes_lengths));
		throw Exception(message + ::format("\n {}", e.what()));
	}
}

/// @brief Reader-friendly smart element reordering for multidimensional tensors.
/// This operation includes functionality of transpose (axes permutation), reshape (view), 
/// squeeze, unsqueeze, stack, concatenate and other operations.
/// @param tensor tensor of any supported library (only libtorch in this version)
/// list of tensors is also accepted, those should be of the same type and shape
/// @param pattern string, rearrangement pattern
/// @param axes_lengths any additional specifications for dimensions
/// @return tensor of the same type as input.
template <typename Tensor, typename... Args>
auto rearrange(Tensor const& tensor, std::string const& pattern, Args... axes_lengths)
{
	return reduce(tensor, pattern, "rearrange", axes_lengths...);
}

/// @brief Reader-friendly smart element reordering for multidimensional tensors.
/// This operation includes functionality of repeat, tile, broadcast functions.
/// @param tensor tensor of any supported library (only libtorch in this version)
/// list of tensors is also accepted, those should be of the same type and shape
/// @param pattern string, rearrangement pattern
/// @param axes_lengths any additional specifications for dimensions
/// @return tensor of the same type as input.
template <typename Tensor, typename... Args>
auto repeat(Tensor const& tensor, std::string const& pattern, Args... axes_lengths)
{
	return reduce(tensor, pattern, "repeat", axes_lengths...);
}

/// @brief Calls einsum operations with einops-style named axes indexing,
/// computing tensor products with an arbitrary number of tensors. 
/// Unlike python version, you need to pass pattern first, ant then the tensor(s).
/// @param pattern string in einops-style.
/// @param tensor one or more tensor where is type is supported by the backends.
/// @return tensor of the same type as input.
template <typename... Tensor>
auto einsum(std::string const& pattern, Tensor... tensor)
{
	static_assert(sizeof...(Tensor) > 0, "einsum() needs at least one Tensor after pattern");
	using namespace implementation;
	auto&& tensors = ::to_vector(std::tuple<Tensor...>(tensor...));
	auto&& [backend, _] = backends::get_backend(tensors[0]);
	return backend.einsum(_compactify_pattern_for_einsum(pattern), tensors);
}

/// @brief Parse a tensor shape to dictionary mapping axes names to their lengths.
/// @param tensor tensor of any supported library
/// @param pattern string, space separated names for axes, underscore means skip axis
/// @return map of axes names to their lengths
template <typename Tensor>
inline auto parse_shape(Tensor const& tensor, std::string const& pattern)
{
	using namespace implementation;
	auto exp = ParsedExpression(pattern, true);
	auto [backend, _] = backends::get_backend(tensor);
	Shape shape = backend.shape(tensor);
	if (exp.has_composed_axes())
		throw std::runtime_error(format("Can't parse shape with composite axes: {} {}", pattern, print(shape)));
	auto flat_composition = [](Composition const& comp) -> FlatComposition
	{
		FlatComposition composition;
		for (auto&& value : comp)
			if (value.index() == 0)
				composition.push_back(std::get<0>(value).front());
		return composition;
	};
	auto composition = flat_composition(exp.composition);
	if (shape.size() != exp.composition.size())
	{
		if (exp.has_ellipsis)
		{
			if (shape.size() < exp.composition.size() - 1)
				throw std::runtime_error(format("Can't parse shape with this number of dimensions: {} {}", pattern, print(shape)));
		}
		else
			throw std::runtime_error(format("Can't parse shape with different number of dimensions: {} {}", pattern, print(shape)));
	}
	if (exp.has_ellipsis)
	{
		size_t ellipsis_idx = index(composition, _ellipsis);
						     remove(composition,  ellipsis_idx);
		for (auto i : iters::range(shape.size() - composition.size() + 1))
			insert(composition, ellipsis_idx, "_");

	}
	AxesLengthsMap result;
	for (auto [axis_name, axis_length] : iters::zip(composition, shape))
	{
		if (axis_name != "_")
			result[axis_name] = axis_length;
	}
	return result;
}

/// @brief Helper for simulating the axes lengths definition in python (e.g. 'ax=5').
/// @param key string of the axis identifier 
/// @param value integer of the axis length
/// @return tuple holding the key and value.
inline auto axis(std::string const& key, int64_t const value)
{
	return std::make_tuple(key, value);
}

} // namespace einops
