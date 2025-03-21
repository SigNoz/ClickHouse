#pragma once

#include <Core/Field.h>
#include <Core/NamesAndTypes.h>
#include <Core/ColumnsWithTypeAndName.h>
#include <Columns/IColumn.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

class IFunctionBase;
using FunctionBasePtr = std::shared_ptr<const IFunctionBase>;

/** A column containing a lambda expression.
  * Behaves like a constant-column. Contains an expression, but not input or output data.
  */
class ColumnFunction final : public COWHelper<IColumn, ColumnFunction>
{
private:
    friend class COWHelper<IColumn, ColumnFunction>;

    ColumnFunction(
        size_t size,
        FunctionBasePtr function_,
        const ColumnsWithTypeAndName & columns_to_capture,
        bool is_short_circuit_argument_ = false,
        bool is_function_compiled_ = false,
        bool recursively_convert_result_to_full_column_if_low_cardinality_ = false);

public:
    const char * getFamilyName() const override { return "Function"; }
    TypeIndex getDataType() const override { return TypeIndex::Function; }

    MutableColumnPtr cloneResized(size_t size) const override;

    size_t size() const override { return elements_size; }

    ColumnPtr cut(size_t start, size_t length) const override;
    ColumnPtr replicate(const Offsets & offsets) const override;
    ColumnPtr filter(const Filter & filt, ssize_t result_size_hint) const override;
    void expand(const Filter & mask, bool inverted) override;
    ColumnPtr permute(const Permutation & perm, size_t limit) const override;
    ColumnPtr index(const IColumn & indexes, size_t limit) const override;

    std::vector<MutableColumnPtr> scatter(IColumn::ColumnIndex num_columns,
                                          const IColumn::Selector & selector) const override;

    void getExtremes(Field &, Field &) const override {}

    size_t byteSize() const override;
    size_t byteSizeAt(size_t n) const override;
    size_t allocatedBytes() const override;

    void appendArguments(const ColumnsWithTypeAndName & columns);
    ColumnWithTypeAndName reduce() const;

    Field operator[](size_t) const override
    {
        throw Exception("Cannot get value from " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void get(size_t, Field &) const override
    {
        throw Exception("Cannot get value from " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    StringRef getDataAt(size_t) const override
    {
        throw Exception("Cannot get value from " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    bool isDefaultAt(size_t) const override
    {
        throw Exception("isDefaultAt is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void insert(const Field &) override
    {
        throw Exception("Cannot insert into " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void insertDefault() override
    {
        throw Exception("Cannot insert into " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void insertFrom(const IColumn & src, size_t n) override;
    void insertRangeFrom(const IColumn &, size_t start, size_t length) override;

    void insertData(const char *, size_t) override
    {
        throw Exception("Cannot insert into " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    StringRef serializeValueIntoArena(size_t, Arena &, char const *&) const override
    {
        throw Exception("Cannot serialize from " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    const char * deserializeAndInsertFromArena(const char *) override
    {
        throw Exception("Cannot deserialize to " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    const char * skipSerializedInArena(const char*) const override
    {
        throw Exception("Cannot skip serialized " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void updateHashWithValue(size_t, SipHash &) const override
    {
        throw Exception("updateHashWithValue is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void updateWeakHash32(WeakHash32 &) const override
    {
        throw Exception("updateWeakHash32 is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void updateHashFast(SipHash &) const override
    {
        throw Exception("updateHashFast is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void popBack(size_t) override
    {
        throw Exception("popBack is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    int compareAt(size_t, size_t, const IColumn &, int) const override
    {
        throw Exception("compareAt is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void compareColumn(const IColumn &, size_t, PaddedPODArray<UInt64> *, PaddedPODArray<Int8> &, int, int) const override
    {
        throw Exception("compareColumn is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    bool hasEqualValues() const override
    {
        throw Exception("hasEqualValues is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void getPermutation(IColumn::PermutationSortDirection, IColumn::PermutationSortStability,
                        size_t, int, Permutation &) const override
    {
        throw Exception("getPermutation is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void updatePermutation(IColumn::PermutationSortDirection, IColumn::PermutationSortStability,
                        size_t, int, Permutation &, EqualRanges &) const override
    {
        throw Exception("updatePermutation is not implemented for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void gather(ColumnGathererStream &) override
    {
        throw Exception("Method gather is not supported for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    double getRatioOfDefaultRows(double) const override
    {
        throw Exception("Method getRatioOfDefaultRows is not supported for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    void getIndicesOfNonDefaultRows(Offsets &, size_t, size_t) const override
    {
        throw Exception("Method getIndicesOfNonDefaultRows is not supported for " + getName(), ErrorCodes::NOT_IMPLEMENTED);
    }

    bool isShortCircuitArgument() const { return is_short_circuit_argument; }

    DataTypePtr getResultType() const;

    /// Create copy of this column, but with recursively_convert_result_to_full_column_if_low_cardinality = true
    ColumnPtr recursivelyConvertResultToFullColumnIfLowCardinality() const;

private:
    size_t elements_size;
    FunctionBasePtr function;
    ColumnsWithTypeAndName captured_columns;

    /// Determine if it's used as a lazy executed argument for short-circuit function.
    /// It's needed to distinguish between lazy executed argument and
    /// argument with ColumnFunction column (some functions can return it)
    /// See ExpressionActions.cpp for details.
    bool is_short_circuit_argument;

    /// Special flag for lazy executed argument for short-circuit function.
    /// If true, call recursiveRemoveLowCardinality on the result column
    /// when function will be executed.
    /// It's used when short-circuit function uses default implementation
    /// for low cardinality arguments.
    bool recursively_convert_result_to_full_column_if_low_cardinality = false;

    /// Determine if passed function is compiled. Used for profiling.
    bool is_function_compiled;

    void appendArgument(const ColumnWithTypeAndName & column);

    void addOffsetsForReplication(const IColumn::Offsets & offsets);
};

const ColumnFunction * checkAndGetShortCircuitArgument(const ColumnPtr & column);

}
