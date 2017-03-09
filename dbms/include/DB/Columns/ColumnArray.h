#pragma once

#include <string.h> // memcpy

#include <DB/Common/Exception.h>
#include <DB/Common/Arena.h>
#include <DB/Common/SipHash.h>

#include <DB/Columns/IColumn.h>
#include <DB/Columns/ColumnsNumber.h>
#include <DB/Columns/ColumnString.h>


namespace DB
{

namespace ErrorCodes
{
	extern const int ILLEGAL_COLUMN;
	extern const int NOT_IMPLEMENTED;
	extern const int BAD_ARGUMENTS;
}

/** A column of array values.
  * In memory, it is represented as one column of a nested type, whose size is equal to the sum of the sizes of all arrays,
  *  and as an array of offsets in it, which allows you to get each element.
  */
class ColumnArray final : public IColumn
{
public:
	/** On the index i there is an offset to the beginning of the i + 1 -th element. */
	using ColumnOffsets_t = ColumnVector<Offset_t>;

	/** Create an empty column of arrays with the type of values as in the column `nested_column` */
	explicit ColumnArray(ColumnPtr nested_column, ColumnPtr offsets_column = nullptr)
		: data(nested_column), offsets(offsets_column)
	{
		if (!offsets_column)
		{
			offsets = std::make_shared<ColumnOffsets_t>();
		}
		else
		{
			if (!typeid_cast<ColumnOffsets_t *>(&*offsets_column))
				throw Exception("offsets_column must be a ColumnUInt64", ErrorCodes::ILLEGAL_COLUMN);
		}
	}

	std::string getName() const override { return "ColumnArray(" + getData().getName() + ")"; }

	ColumnPtr cloneResized(size_t size) const override
	{
		ColumnPtr new_col_holder = std::make_shared<ColumnArray>(getData().cloneEmpty());

		if (size > 0)
		{
			auto & new_col = static_cast<ColumnArray &>(*new_col_holder);
			size_t count = std::min(this->size(), size);

			/// First create the offsets.
			const auto & from_offsets = getOffsets();
			auto & new_offsets = new_col.getOffsets();
			new_offsets.resize(size);
			new_offsets.assign(from_offsets.begin(), from_offsets.begin() + count);

			if (size > count)
			{
				for (size_t i = count; i < size; ++i)
					new_offsets[i] = new_offsets[i - 1];
			}

			/// Then store the data.
			new_col.getData().insertRangeFrom(getData(), 0, count);
		}

		return new_col_holder;
	}

	size_t size() const override
	{
		return getOffsets().size();
	}

	Field operator[](size_t n) const override
	{
		size_t offset = offsetAt(n);
		size_t size = sizeAt(n);
		Array res(size);

		for (size_t i = 0; i < size; ++i)
			res[i] = getData()[offset + i];

		return res;
	}

	void get(size_t n, Field & res) const override
	{
		size_t offset = offsetAt(n);
		size_t size = sizeAt(n);
		res = Array(size);
		Array & res_arr = DB::get<Array &>(res);

		for (size_t i = 0; i < size; ++i)
			getData().get(offset + i, res_arr[i]);
	}

	StringRef getDataAt(size_t n) const override
	{
		/** Returns the range of memory that covers all elements of the array.
		  * Works for arrays of fixed length values.
		  * For arrays of strings and arrays of arrays, the resulting chunk of memory may not be one-to-one correspondence with the elements,
		  *  since it contains only the data laid in succession, but not the offsets.
		  */

		size_t array_size = sizeAt(n);
		if (array_size == 0)
			return StringRef();

		size_t offset_of_first_elem = offsetAt(n);
		StringRef first = getData().getDataAtWithTerminatingZero(offset_of_first_elem);

		size_t offset_of_last_elem = getOffsets()[n] - 1;
		StringRef last = getData().getDataAtWithTerminatingZero(offset_of_last_elem);

		return StringRef(first.data, last.data + last.size - first.data);
	}

	void insertData(const char * pos, size_t length) override
	{
		/** Similarly - only for arrays of fixed length values.
		  */
		IColumn * data_ = data.get();
		if (!data_->isFixed())
			throw Exception("Method insertData is not supported for " + getName(), ErrorCodes::NOT_IMPLEMENTED);

		size_t field_size = data_->sizeOfField();

		const char * end = pos + length;
		size_t elems = 0;
		for (; pos + field_size <= end; pos += field_size, ++elems)
			data_->insertData(pos, field_size);

		if (pos != end)
			throw Exception("Incorrect length argument for method ColumnArray::insertData", ErrorCodes::BAD_ARGUMENTS);

		getOffsets().push_back((getOffsets().size() == 0 ? 0 : getOffsets().back()) + elems);
	}

	StringRef serializeValueIntoArena(size_t n, Arena & arena, char const *& begin) const override
	{
		size_t array_size = sizeAt(n);
		size_t offset = offsetAt(n);

		char * pos = arena.allocContinue(sizeof(array_size), begin);
		memcpy(pos, &array_size, sizeof(array_size));

		size_t values_size = 0;
		for (size_t i = 0; i < array_size; ++i)
			values_size += getData().serializeValueIntoArena(offset + i, arena, begin).size;

		return StringRef(begin, sizeof(array_size) + values_size);
	}

	const char * deserializeAndInsertFromArena(const char * pos) override
	{
		size_t array_size = *reinterpret_cast<const size_t *>(pos);
		pos += sizeof(array_size);

		for (size_t i = 0; i < array_size; ++i)
			pos = getData().deserializeAndInsertFromArena(pos);

		getOffsets().push_back((getOffsets().size() == 0 ? 0 : getOffsets().back()) + array_size);
		return pos;
	}

	void updateHashWithValue(size_t n, SipHash & hash) const override
	{
		size_t array_size = sizeAt(n);
		size_t offset = offsetAt(n);

		hash.update(reinterpret_cast<const char *>(&array_size), sizeof(array_size));
		for (size_t i = 0; i < array_size; ++i)
			getData().updateHashWithValue(offset + i, hash);
	}

	void insertRangeFrom(const IColumn & src, size_t start, size_t length) override;

	void insert(const Field & x) override
	{
		const Array & array = DB::get<const Array &>(x);
		size_t size = array.size();
		for (size_t i = 0; i < size; ++i)
			getData().insert(array[i]);
		getOffsets().push_back((getOffsets().size() == 0 ? 0 : getOffsets().back()) + size);
	}

	void insertFrom(const IColumn & src_, size_t n) override
	{
		const ColumnArray & src = static_cast<const ColumnArray &>(src_);
		size_t size = src.sizeAt(n);
		size_t offset = src.offsetAt(n);

		getData().insertRangeFrom(src.getData(), offset, size);
		getOffsets().push_back((getOffsets().size() == 0 ? 0 : getOffsets().back()) + size);
	}

	void insertDefault() override
	{
		getOffsets().push_back(getOffsets().size() == 0 ? 0 : getOffsets().back());
	}

	void popBack(size_t n) override
	{
		auto & offsets = getOffsets();
		size_t nested_n = offsets.back() - offsetAt(offsets.size() - n);
		if (nested_n)
			getData().popBack(nested_n);
		offsets.resize_assume_reserved(offsets.size() - n);
	}

	ColumnPtr filter(const Filter & filt, ssize_t result_size_hint) const override;

	ColumnPtr permute(const Permutation & perm, size_t limit) const override;

	int compareAt(size_t n, size_t m, const IColumn & rhs_, int nan_direction_hint) const override
	{
		const ColumnArray & rhs = static_cast<const ColumnArray &>(rhs_);

		/// Not optimal
		size_t lhs_size = sizeAt(n);
		size_t rhs_size = rhs.sizeAt(m);
		size_t min_size = std::min(lhs_size, rhs_size);
		for (size_t i = 0; i < min_size; ++i)
			if (int res = getData().compareAt(offsetAt(n) + i, rhs.offsetAt(m) + i, *rhs.data.get(), nan_direction_hint))
				return res;

		return lhs_size < rhs_size
			? -1
			: (lhs_size == rhs_size
				? 0
				: 1);
	}

	template <bool positive>
	struct less
	{
		const ColumnArray & parent;

		less(const ColumnArray & parent_) : parent(parent_) {}

		bool operator()(size_t lhs, size_t rhs) const
		{
			if (positive)
				return parent.compareAt(lhs, rhs, parent, 1) < 0;
			else
				return parent.compareAt(lhs, rhs, parent, -1) > 0;
		}
	};

	void getPermutation(bool reverse, size_t limit, Permutation & res) const override;

	void reserve(size_t n) override
	{
		getOffsets().reserve(n);
		getData().reserve(n);       /// The average size of arrays is not taken into account here. Or it is considered to be no more than 1.
	}

	size_t byteSize() const override
	{
		return getData().byteSize() + getOffsets().size() * sizeof(getOffsets()[0]);
	}

	size_t allocatedSize() const override
	{
		return getData().allocatedSize() + getOffsets().allocated_size() * sizeof(getOffsets()[0]);
	}

	bool hasEqualOffsets(const ColumnArray & other) const
	{
		if (offsets == other.offsets)
			return true;

		const Offsets_t & offsets1 = getOffsets();
		const Offsets_t & offsets2 = other.getOffsets();
		return offsets1.size() == offsets2.size() && 0 == memcmp(&offsets1[0], &offsets2[0], sizeof(offsets1[0]) * offsets1.size());
	}

	/** More efficient methods of manipulation */
	IColumn & getData() { return *data.get(); }
	const IColumn & getData() const { return *data.get(); }

	ColumnPtr & getDataPtr() { return data; }
	const ColumnPtr & getDataPtr() const { return data; }

	Offsets_t & ALWAYS_INLINE getOffsets()
	{
		return static_cast<ColumnOffsets_t &>(*offsets.get()).getData();
	}

	const Offsets_t & ALWAYS_INLINE getOffsets() const
	{
		return static_cast<const ColumnOffsets_t &>(*offsets.get()).getData();
	}

	ColumnPtr & getOffsetsColumn() { return offsets; }
	const ColumnPtr & getOffsetsColumn() const { return offsets; }


	ColumnPtr replicate(const Offsets_t & replicate_offsets) const override;

	Columns scatter(ColumnIndex num_columns, const Selector & selector) const override
	{
		return scatterImpl<ColumnArray>(num_columns, selector);
	}

	ColumnPtr convertToFullColumnIfConst() const override
	{
		ColumnPtr new_data;
		ColumnPtr new_offsets;

		if (auto full_column = getData().convertToFullColumnIfConst())
			new_data = full_column;
		else
			new_data = data;

		if (auto full_column = offsets.get()->convertToFullColumnIfConst())
			new_offsets = full_column;
		else
			new_offsets = offsets;

		return std::make_shared<ColumnArray>(new_data, new_offsets);
	}

	void getExtremes(Field & min, Field & max) const override
	{
		min = Array();
		max = Array();
	}

private:
	ColumnPtr data;
	ColumnPtr offsets;  /// Displacements can be shared across multiple columns - to implement nested data structures.

	size_t ALWAYS_INLINE offsetAt(size_t i) const	{ return i == 0 ? 0 : getOffsets()[i - 1]; }
	size_t ALWAYS_INLINE sizeAt(size_t i) const		{ return i == 0 ? getOffsets()[0] : (getOffsets()[i] - getOffsets()[i - 1]); }


	/// Multiply values if the nested column is ColumnVector<T>.
	template <typename T>
	ColumnPtr replicateNumber(const Offsets_t & replicate_offsets) const;

	/// Multiply the values if the nested column is ColumnString. The code is too complicated.
	ColumnPtr replicateString(const Offsets_t & replicate_offsets) const;

	/** Non-constant arrays of constant values are quite rare.
	  * Most functions can not work with them, and does not create such columns as a result.
	  * An exception is the function `replicate`(see FunctionsMiscellaneous.h), which has service meaning for the implementation of lambda functions.
	  * Only for its sake is the implementation of the `replicate` method for ColumnArray(ColumnConst).
	  */
	ColumnPtr replicateConst(const Offsets_t & replicate_offsets) const;


	/// Specializations for the filter function.
	template <typename T>
	ColumnPtr filterNumber(const Filter & filt, ssize_t result_size_hint) const;

	ColumnPtr filterString(const Filter & filt, ssize_t result_size_hint) const;
	ColumnPtr filterGeneric(const Filter & filt, ssize_t result_size_hint) const;
};


}
