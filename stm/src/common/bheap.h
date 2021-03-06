#ifndef MSN_BHEAP_H
#define MSN_BHEAP_H

#include <stdint.h>
#include <string.h>
#include <type_traits>
#include <iterator>

#if __cpp_inline_variables >= 201603
#define MSN_BHEAP_INLINE_V const inline static
#else
#define MSN_BHEAP_INLINE_V constexpr static
#endif

// "Block-HEAP"
//
// The datastructure which implements stuff in the README
namespace bheap {

	template<typename T>
	struct TypedBlock;
	template<size_t Size>
	struct Arena;

	// Represents a block header
	struct Block {
		uint32_t slotid : 12;
		uint32_t datasize : 14;
		uint32_t temperature : 2;
		uint32_t location : 2;
		uint32_t flags : 2;

		MSN_BHEAP_INLINE_V uint32_t TemperatureHot = 0b11;
		MSN_BHEAP_INLINE_V uint32_t TemperatureWarm = 0b10;
		MSN_BHEAP_INLINE_V uint32_t TemperatureCold = 0b01;

		MSN_BHEAP_INLINE_V uint32_t LocationCanonical = 0b01;
		MSN_BHEAP_INLINE_V uint32_t LocationEphemeral = 0b10;
		MSN_BHEAP_INLINE_V uint32_t LocationRemote = 0b11;

		MSN_BHEAP_INLINE_V uint32_t SlotEmpty = 0xff0;
		MSN_BHEAP_INLINE_V uint32_t SlotEnd = 0xff1;

		MSN_BHEAP_INLINE_V uint32_t FlagDirty = 1;
		MSN_BHEAP_INLINE_V uint32_t FlagFlush = 2;

		// DATA ACCESS

		// Get the next block (4 byte alignment)
		const Block *adjacent() const {
			if (this->slotid == SlotEnd)
				return nullptr;
			if (this->location == LocationRemote && *this)
				return this + 1;
			if (datasize % 4) {
				return this + 2 + (datasize / 4);
			} 
			else {
				return this + 1 + (datasize / 4);
			}
		}

		Block *adjacent() {
			if (this->slotid == SlotEnd)
				return nullptr;
			if (this->location == LocationRemote && *this)
				return this + 1;
			if (datasize % 4) {
				return this + 2 + (datasize / 4);
			} 
			else {
				return this + 1 + (datasize / 4);
			}
		}

		// Get the next block of the same type (4 byte alignment)
		const Block *next() const {
			const Block *adj = this;
			while ((adj = adj->adjacent()) != nullptr && adj->slotid != this->slotid) {}
			return adj;
		}

		Block * next() {
			Block * adj = this;
			while ((adj = adj->adjacent()) != nullptr && adj->slotid != this->slotid) {}
			return adj;
		}

		// Get the data of this block
		void * data() {
			return (this + 1);
		}

		// Get the data of this block
		const void * data() const {
			return (this + 1);
		}

		// Get this as a typedblock
		template<typename T>
		TypedBlock<T>& as() {
			return *reinterpret_cast<TypedBlock<T>*>(this);
		}

		// Get this as a typedblock
		template<typename T>
		const TypedBlock<T>& as() const {
			return *reinterpret_cast<const TypedBlock<T>*>(this);
		}

		// MUTATION

		// Shrink this block, adding an empty block if necessary.
		// This is invalid for end blocks.
		// For empty blocks this will effectively split them into two segments.

		void shrink(uint32_t newsize) {
			// Store the previous next block
			auto prev = adjacent();
			// Do the shrink
			this->datasize = newsize;
			if (adjacent() != prev) {
				// Create a new block with size = prev - next() - 4
				//
				// Since prev and next are aligned to 4 bytes, the
				// next() ptr will align properly
				*adjacent() = Block{};
				adjacent()->slotid = SlotEmpty;
				adjacent()->datasize = (reinterpret_cast<uintptr_t>(prev) - reinterpret_cast<uintptr_t>(adjacent()) - 4);
			}
		}

		// Insert a new block. Only valid for empty blocks.
		// Allows for inserting at any offset, but doesn't check for errors.
		//
		// Use will_fit for that. (or build with asserts on or something)
		//
		// Offset is relative to the block header (i.e. offset = 0 will overwrite the block, offset = 4 will zero-size this block, etc.)
		void insert(const Block& b, uint32_t offset) {
			if (offset % 4) { // get angry
				return;
			}
			// Use the power of recursion to make this only have to deal with the case for offset = 0
			if (offset != 0) {
				// Shrink this block to size = offset - 4
				this->shrink(offset - 4);
				// Overwrite next
				adjacent()->insert(b, 0);
			}
			else {
				// Shrink this block so that there's a new end.
				this->shrink((b && b.location == LocationRemote) ? 0 : b.datasize);
				// Overwrite this
				*this = b;
			}
		}

		// HELPERS

		// Check if that block will fit in this location
		bool will_fit(const Block& b, uint32_t offset) const {
			uint32_t length = offset + (b.adjacent() - &b) * 4;
			return length <= (adjacent() - this) * 4;
		}

		operator bool() const {
			return this->slotid != SlotEnd && this->slotid != SlotEmpty;
		}

		template<size_t> friend struct Arena;
	private:

		Block& operator=(const Block& other) = default;
		Block& operator=(Block&& other) = default;
		Block(const Block& other) = default;
		Block(Block&& other) = default;
		Block() = default;
	};

	static_assert(sizeof(Block) == 4, "Block is not 4 bytes");

	// Helper type to access blocks of a known type
	template<typename T>
	struct TypedBlock : protected Block {
#if __cpp_if_constexpr >= 201603
		using data_type = std::conditional_t<std::is_pointer<std::decay_t<T>>::value, T, T&>;
		using const_data_type = std::conditional_t<std::is_pointer<std::decay_t<T>>::value, std::add_pointer_t<const std::remove_pointer_t<T>>, const T&>;

		// Get the data of this block.
		// If T is a pointer type, return a pointer to that type at the beginning of the data block (useful for variable length arrays / strings)
		const_data_type data() const {
			if constexpr (std::is_pointer_v<std::decay_t<T>>) {
				return reinterpret_cast<std::add_pointer_t<const std::remove_pointer_t<T>>>(Block::data());
			}
			else {
				return *reinterpret_cast<std::add_pointer_t<const std::decay_t<T>>>(Block::data());
			}
		}

		data_type data() {
			if constexpr (std::is_pointer_v<std::decay_t<T>>) {
				return reinterpret_cast<T>(Block::data());
			}
			else {
				return *reinterpret_cast<std::add_pointer_t<std::decay_t<T>>>(Block::data());
			}
		}

		// Allow use as a pointer type
		T* operator->() {return &data();}
		const T* operator->() const {return &data();}
		data_type operator*() {return data();}
		const_data_type operator*() const {return data();}
		template<typename Idx>
		auto operator[](Idx idx) const {return (*(*this))[idx];}
		template<typename Idx>
		auto operator[](Idx idx) {return (*(*this))[idx];}
#endif

		using Block::next;

		// Allow access to the metadata
		using Block::datasize;
		using Block::slotid;
		using Block::location;
		using Block::temperature;

		operator bool() const {
			return Block::operator bool() && this->datasize;
		}
	};

	// An arena of blocks (fight!)
	template<size_t Size>
	struct Arena {
		static_assert(Size % 4 == 0, "Arena must have a size aligned to 4 bytes");

		template<typename T, bool Adjacent>
		struct Iterator {
			static_assert(!(Adjacent && !std::is_same<typename std::decay<T>::type, Block>::value), "Using Arena::Iterator in adjacent mode on TypedBlocks is invalid.");

			using iterator_category = std::forward_iterator_tag;
			using value_type = T;
			using difference_type = ptrdiff_t;
			using reference = T&;
			using pointer = T*;

			Iterator(T& f) {
				ptr = &f;
			}

			Iterator() {
				ptr = nullptr;
			}

			pointer operator->() const {return ptr;}
			reference operator*() const {return *ptr;}

			Iterator& operator++() {
#if __cpp_if_constexpr >= 201603
				if constexpr (Adjacent)
					ptr = ptr->adjacent();
				else
					ptr = ptr->next();
#else
				ptr = Adjacent ? ptr->adjacent() : ptr->next();
#endif
				return *this;
			}

			Iterator operator++(int) {
				// Copy
				auto copy = *this;
				++this;
				return copy;
			}

			operator Iterator<const T, Adjacent>() const {
				return Iterator<const T, Adjacent>(*ptr);
			}

			bool operator==(const Iterator& other) const {
				return other.ptr == ptr;
			}

			bool operator!=(const Iterator& other) const {
				return other.ptr != ptr;
			}

			operator T*() const {
				return ptr;
			}

		private:
			T* ptr;
		};

		static_assert(Size >= 8, "Not large enough to hold an end and empty marker block");

		union {
			Block first;
			uint8_t region[Size]{};
		};

		Arena() {
			first.slotid = Block::SlotEmpty;
			first.datasize = Size - 4;

			// Create a handy dandy end block
			first.shrink(Size - 8);
			*first.adjacent() = Block();
			first.adjacent()->datasize = 0;
			first.adjacent()->slotid = Block::SlotEnd;
		}

		// MODIFICATION OPERATIONS

		// Add a new block of this size somewhere after the last block of this slot.
		//
		// If this is the first block, the temperature is initialized to cold, otherwise it is kept correct
		bool add_block(uint32_t slotid, uint32_t datalocation, size_t datasize) {
			// First, check if there's enough space to fit in this block.
			uint32_t free_space = this->free_space();
			if (contains(slotid)) {
				free_space = this->free_space(this->get(slotid), FreeSpaceAllocatable); // explicitly use the second one to avoid a warning
			}

			uint32_t required_space = 4;
			if (datalocation != Block::LocationRemote) required_space += datasize;

			if (required_space > free_space) {
				// TODO: try to relocate the blocks earlier (if there is space earlier, anyways)
				return false;
			}

			// There is space somewhere after the last block.
			// Find the next free space. We always allocate at the beginning of free spaces.

			Block newb;
			newb.slotid = slotid;
			newb.datasize = static_cast<uint32_t>(datasize);
			newb.temperature = Block::TemperatureCold;
			newb.location = datalocation;
			newb.flags = 0;

			if (contains(slotid)) newb.temperature = get(slotid).temperature;

			for (Block* x = contains(slotid) ? &last(slotid) : &first; x->adjacent(); x = x->adjacent()) {
				if (x->slotid == Block::SlotEnd) return false; // end; fragmented
				if (*x) continue; // non-empty
				// empty block, is there space?
				if (x->will_fit(newb, 0)) {
					// place this block there
					x->insert(newb, 0);
					return true;
				}
			}

			// If we're here, then there isn't a big enough empty block to place this chunk.

			return false; // Broken heap
		}

		// Update the contents of the data at that offset + length with data.
		//
		// This function will convert remote chunks to either canonical + flush or ephemeral (semantically the correct choice)
		bool update_contents(uint32_t slotid, uint32_t offset, uint32_t length, const void *data, bool set_flush=false) {
			if (offset + length > contents_size(slotid)) return false;
			// Check if the region crosses the boundary between two segments, if so, split the function into two calls.
			Block& containing_block = get(slotid, offset);
			auto begin_pos = block_offset(containing_block);
			if (offset + length > begin_pos + containing_block.datasize) {
				// Region starting at offset has size begin_pos + containing_block.datasize + 1 - offset
				// | x | |
				// 0123456
				// Region starting at begin_pos + containing_block.datasize has size offset + length - begin_pos + containing_block.datasize
				auto sublength = begin_pos + containing_block.datasize - offset;
				return update_contents(slotid, offset, begin_pos + containing_block.datasize - offset, data, set_flush) &&
					   update_contents(slotid, offset + sublength, length - sublength, static_cast<const uint8_t*>(data) + sublength, set_flush);
			}

			// Check if this is a remote region, in which case we have to convert it
			if (containing_block.location == Block::LocationRemote) {
				// Set the location
				if (!set_location(slotid, offset, length, set_flush ? Block::LocationCanonical : Block::LocationEphemeral)) return false;
				Block& new_block = get(slotid, offset);
				new_block.flags = (set_flush ? Block::FlagFlush : Block::FlagDirty);
				// Tail recursion
				return update_contents(slotid,offset, length, data, set_flush);
			}
			
			// Otherwise, just patch the block content
			memcpy((uint8_t *)(containing_block.data()) + (offset - begin_pos), data, length);
			containing_block.flags |= Block::FlagDirty;
			return true;
		}

		// Check if the range given is stored at the given location.
		bool check_location(uint32_t slotid, uint32_t offset, uint32_t length, uint32_t location) const {
			if (offset + length > contents_size(slotid)) return false;
			
			// This isn't particularly fast but probably has less problems than the old implementation
			for (auto x = cbegin(slotid); x != cend(slotid); ++x) {
				if ((offset+length) <= block_offset(*x) || offset > (block_offset(*x) + x->datasize)) continue;
				if (x->location != location) return false;
			}

			return true;
		}

		// Change the storage location of a given region
		//
		// If told to convert to remote, will create the necessary block.
		// If told to convert _from_ remote, will create necessary blocks and shrink/replace remote sections.
		bool set_location(uint32_t slotid, uint32_t offset, uint32_t length, uint32_t location) {
			if (offset + length > contents_size(slotid)) return false;
			// Strategy for normal locations:
			// 	- find beginning block and ending block (min max, possibly overshooting)
			// 	- over all "inner" blocks just change value
			// 	- if begin == end:
			// 		- split block up (moving over 4 bytes)
			// 		- add new block
			// 		- split again (moving over another 4 bytes)
			// 	- else:
			// 		- split block only once
			// Strategy for remote blocks:
			// 	- find beginning and ending block, split if required
			// 	- nullify all inner blocks
			// 	- insert remote block

			// Check if the region crosses the boundary between two segments, if so, split the function into two calls.
			{
				Block& containing_block = get(slotid, offset);
				auto begin_pos = block_offset(containing_block);
				if (offset + length > begin_pos + containing_block.datasize) {
					// Region starting at offset has size begin_pos + containing_block.datasize + 1 - offset
					// | x | |
					// 0123456
					// Region starting at begin_pos + containing_block.datasize has size offset + length - begin_pos + containing_block.datasize
					auto sublength = begin_pos + containing_block.datasize - offset;
					return set_location(slotid, offset, begin_pos + containing_block.datasize - offset, location) &&
						   set_location(slotid, offset + sublength, length - sublength, location);
				}

				// Check if the region is not the entire block
				if (begin_pos != offset) {
					// Split block once
					if (!split_block(containing_block, offset - begin_pos)) return false;
				}
			}
			{
				Block& containing_block = get(slotid, offset);
				auto begin_pos = block_offset(containing_block);

				// Check if the region is not the entire block (end)
				if (begin_pos + containing_block.datasize != offset + length) {
					// Split block once again
					if (!split_block(containing_block, (offset + length) - begin_pos)) return false;
				}
			}

			Block& containing_block = get(slotid, offset);

			if (containing_block.location == location) return true;

			if (location == Block::LocationRemote) {
				// shrink block to 0
				containing_block.shrink(0);
				containing_block.datasize = length;
				goto finish_setting;
			}
			else {
				if (containing_block.location == Block::LocationRemote) {
					// Allocate space for entire chunk.
					// I will admit this wastes 4 bytes, but eh it's the easiest and cleanest way to do this
					
					// Delete block contents
					containing_block.shrink(0);

					// Ensure space exists for the block
					if (!make_space_after(containing_block, length)) return false;

					// Copy relevant portions of block info
					auto& new_contain = *containing_block.adjacent();
					new_contain.slotid = slotid;
					new_contain.temperature = containing_block.temperature;
					new_contain.datasize = length;
					new_contain.flags = containing_block.flags;
					new_contain.location = location;

					// Null out old contents
					containing_block.slotid = Block::SlotEmpty;
					return true;
				}
				else {
finish_setting:
					containing_block.location = location;
					return true;
				}
			}
		}

		// Set the temperature of data
		bool set_temperature(uint32_t slotid, uint32_t temperature) {
			if (!contains(slotid)) return false;
			// Change the temperature of all blocks
			for (auto x = begin(slotid); x != end(slotid); ++x) {
				x->temperature = temperature;
			}
			return true;
		}

		// Trim data blocks so that the total size is truncated
		bool truncate_contents(uint32_t slotid, uint32_t size) {
			if (contents_size(slotid) < size) return false; // also does npos
			// if the actual size == size, we just do nothing
			while (contents_size(slotid) > size) {
				auto& endptr = last(slotid);
				uint32_t should_reclaim = contents_size(slotid) - size;
				if (should_reclaim >= endptr.datasize) {
					// delete the block
					if (endptr.location == Block::LocationRemote) endptr.datasize = 0;
					endptr.slotid = Block::SlotEmpty;
				}
				else {
					// truncate 
					endptr.shrink(endptr.datasize - should_reclaim);
				}
			}
			return true;
		}

		// Homogenize data: ensure all of its blocks are in order, and (if possible) merge their data together
		// This does it's "best effort":
		//  - it'll try to merge blocks as it can
		//  - it'll also remove 0-size placeholders
		void homogenize(uint32_t slotid) {
			// Remove 0-size placeholders
			for (auto x = this->begin(slotid); x != this->end(slotid); ++x) {
				if (x->next() && x->location == Block::LocationRemote && x->datasize == 0) {
					auto nextptr = x;
					++nextptr;
					x->slotid = Block::SlotEmpty;
					x->location = Block::LocationCanonical;
					x = nextptr;
				}
			}
			// Rearrange all of the blocks
			for (auto x = this->begin(slotid); x != this->end(slotid); ++x) {
				if (x->next()) {
					move_left(*x->next(), *x);
					// Try to merge
					//
					// We can merge if:
					// 	- the location is equal
					// 	- the flush flag is equal (if the dirty flag is unequal, or it)

					while (x->location == x->next()->location && (x->flags & Block::FlagFlush) == (x->next()->flags & Block::FlagFlush)) {
						// If this is a remote block, just increase the size
						if (x->location == Block::LocationRemote) {
							x->datasize += x->next()->datasize;
							x->next()->location = Block::LocationCanonical;
							x->next()->datasize = 0;
							x->next()->slotid = Block::SlotEmpty;
						}
						else {
							// Copy the data
							Block old = *x->next();
							void * append_data = x->next()->data();
							char * append_data_to = ((char*)x->data()) + x->datasize;
							Block *should_end_at = x->next()->adjacent();
							x->datasize += old.datasize;
							// Append data
							memmove(append_data_to, append_data, old.datasize);
							// If we need to insert a block?
							if (x->adjacent() != should_end_at) {
								// Re-update old
								old.slotid = Block::SlotEmpty;
								old.location = Block::LocationCanonical;
								old.temperature = Block::TemperatureCold;
								old.datasize = (should_end_at - x->adjacent())*sizeof(Block) - 4;
								*x->adjacent() = old;
							}
						}

						if (x->next()) move_left(*x->next(), *x);
						else break;
					}
				}
			}
		}

		// Defragment this Arena
		//
		// Does the following actions:
		// 	- Homogenizes all slots
		// 	- Condenses adjacent empties and moves all empty space to the end of the arena.
		// 	   - This can reduce performance but allows for more blocks to be allocated
		void defrag() {
			// Remove unnecessary placeholders
			for (auto& x : *this) {
				if (x && x.datasize == 0 && x.next()) {
					x.datasize = 0;
					x.slotid = Block::SlotEmpty;
					x.location = Block::LocationCanonical;
				}
			}
			// Homogenize all slots
			for (auto& x : *this) {
				if (x && x.next()) homogenize(x.slotid);
			}
			// Condense empty slots (simplifies next section)
			for (auto& x : *this) {
				while (x.adjacent() && x.slotid == Block::SlotEmpty && x.adjacent()->slotid == Block::SlotEmpty) {
					x.datasize += 4 + x.adjacent()->datasize;
					if (x.datasize % 4) x.datasize += (4 - x.datasize % 4);
				}
			}
			auto last_empty = &last(Block::SlotEmpty);
			if (last_empty->slotid != Block::SlotEnd) {
				for (auto& x : *this) {
					if (x.slotid == Block::SlotEmpty && &x != last_empty) {
						// Shift everything from x->adjacent() of size (&last_empty - x->adjacent()) over to x, then move the empty header
						Block *begin_region = x.adjacent();
						Block *copy_to = &x;
						Block old_empty =    *last_empty;

						memmove(copy_to, begin_region, (last_empty - begin_region)*sizeof(Block));
						// Add a new block header
						last_empty -= (begin_region - copy_to);
						*last_empty = old_empty;
						last_empty->datasize += (begin_region - copy_to)*sizeof(Block);
					}
				}
			}
		}

		// DATA ACCESS OPERATIONS

		// Size/offset for something that doesn't exist
		MSN_BHEAP_INLINE_V uint32_t npos = ~0u;

		// Return the offset into the slot id this block is at.
		//
		// If this block is not present in this Arena return npos.
		uint32_t block_offset(const Block& block) const {
			uint32_t pos = 0;
			for (auto x = this->cbegin(block.slotid); x != this->cend(block.slotid); ++x) {
				if (x == &block) return pos;
				pos += x->datasize;
			}
			return npos;
		}

		// Get the total size of a given slotid (returns npos if not found)
		uint32_t contents_size(uint32_t slotid) const {
			uint32_t total = npos;
			for (auto x = cbegin(slotid); x != cend(slotid); ++x) {
				if (total == npos) total = 0;
				total += x->datasize;
			}
			return total;
		}

		// Get the total free space (with various flags for what counts as "free" space)
		MSN_BHEAP_INLINE_V uint32_t FreeSpaceEmpty = 1; // Total size of empty regions + their headers
		MSN_BHEAP_INLINE_V uint32_t FreeSpaceZeroSizeCanonical = 2; // Headers for empty canonical blocks
		MSN_BHEAP_INLINE_V uint32_t FreeSpaceEphemeral = 4; // Headers + data for ephemeral blocks (cold + warm)
		MSN_BHEAP_INLINE_V uint32_t FreeSpaceHomogenizeable = 8; // Headers for noncontiguous regions.

		MSN_BHEAP_INLINE_V uint32_t FreeSpaceAllocatable = FreeSpaceEmpty; // Free space for new allocations
		MSN_BHEAP_INLINE_V uint32_t FreeSpaceCleanup = FreeSpaceAllocatable | FreeSpaceZeroSizeCanonical | FreeSpaceEphemeral; // Free space after removing stuff
		MSN_BHEAP_INLINE_V uint32_t FreeSpaceDefrag = FreeSpaceAllocatable | FreeSpaceHomogenizeable; // Free space after defrag()

		inline uint32_t free_space(uint32_t mode=FreeSpaceAllocatable) const {
			return free_space(first, mode);
		}
		uint32_t free_space(const Block& after, uint32_t mode) const {
			uint32_t total = 0;
			if (mode & FreeSpaceEmpty) {
				for (auto blk = const_iterator(after); blk != cend(); ++blk) {
					if (blk->slotid == Block::SlotEmpty) {
						total += 4 + blk->datasize;
						if (total % 4) total += 4 - (total % 4);
					}
				}
			}
			if (mode & FreeSpaceZeroSizeCanonical) {
				for (auto blk = const_iterator(after); blk != cend(); ++blk) {
					if (blk->slotid < Block::SlotEmpty && blk->location == Block::LocationCanonical && blk->datasize == 0) total += 4 ;
				}
			}
			if (mode & FreeSpaceEphemeral) {
				for (auto blk = const_iterator(after); blk != cend(); ++blk) {
					if (blk->slotid < Block::SlotEmpty && blk->location == Block::LocationEphemeral && blk->temperature < Block::TemperatureHot && !blk->flags) total += 4 + blk->datasize;
				}
			}
			if (mode & FreeSpaceHomogenizeable) {
				for (auto blk = const_iterator(after); blk != cend(); ++blk) {
					if (*blk && blk->next() && blk->location == blk->next()->location && (blk->flags & Block::FlagFlush) == (blk->next()->flags & Block::FlagFlush)) total += 4;
				}
			}
			return total;
		}

		// Is this data ID represented in this arena
		inline bool contains(uint32_t slotid) const {return get(slotid);}

		// Get the first block of that SlotID (returning the end block if not present, whose bool() is false)
		const Block& get(uint32_t slotid) const {
			for (const Block& x : *this) {
				if (x.slotid == slotid || x.slotid == Block::SlotEnd) return x;
			}
			__builtin_unreachable();
		}
		Block& get(uint32_t slotid) {
			for (Block& x : *this) {
				if (x.slotid == slotid || x.slotid == Block::SlotEnd) return x;
			}
			__builtin_unreachable();
		}

		const Block& get(uint32_t slotid, uint32_t offset) const {
			uint32_t pos = 0;
			for (const Block& x : *this) {
				if (x.slotid == Block::SlotEnd) return x;
				if (x.slotid != slotid) continue;
				if (offset >= pos && offset < pos + x.datasize) return x;
				pos += x.datasize;
			}
			__builtin_unreachable();
		}
		Block& get(uint32_t slotid, uint32_t offset) {
			uint32_t pos = 0;
			for (Block& x : *this) {
				if (x.slotid == Block::SlotEnd) return x;
				if (x.slotid != slotid) continue;
				if (offset >= pos && offset < pos + x.datasize) return x;
				pos += x.datasize;
			}
			__builtin_unreachable();
		}

		// Get the last block of that SlotID
		const Block& last(uint32_t slotid) const {
			const Block* ptr = &get(slotid);
			while (ptr->next()) ptr = ptr->next();
			return *ptr;
		}
		Block& last(uint32_t slotid) {
			Block* ptr = &get(slotid);
			while (ptr->next()) ptr = ptr->next();
			return *ptr;
		}

		// Get the first block of that SlotID as a TypedBlock
		template<typename T>
		inline const TypedBlock<T>& get(uint32_t slotid) {return get(slotid).template as<T>();}
		template<typename T>
		inline TypedBlock<T>& get(uint32_t slotid) {return get(slotid).template as<T>();}

		// HELPERS
		inline const Block& operator[](uint32_t slotid) const {return get(slotid);}
		inline Block& operator[](uint32_t slotid) {return get(slotid);}

		// ITERATION
		// Iterate over all blocks

		using iterator = Iterator<Block, true>;
		using const_iterator = Iterator<const Block, true>;

		using nonadj_iterator = Iterator<Block, false>;
		using nonadj_const_iterator = Iterator<const Block, false>;

		inline iterator begin() {return iterator(first);}
		inline const_iterator begin() const {return const_iterator(first);}
		inline const_iterator cbegin() const {return const_iterator(first);}

		inline iterator end() {return iterator();}
		inline const_iterator end() const {return const_iterator();}
		inline const_iterator cend() const {return const_iterator();}

		// Iterate over blocks for slotid without type

		inline nonadj_iterator begin(uint32_t slotid) {return nonadj_iterator(this->get(slotid));}
		inline nonadj_const_iterator begin(uint32_t slotid) const {return nonadj_const_iterator(this->get(slotid));}
		inline nonadj_const_iterator cbegin(uint32_t slotid) const {return nonadj_const_iterator(this->get(slotid));}

		inline nonadj_iterator end(uint32_t slotid) {return nonadj_iterator();}
		inline nonadj_const_iterator end(uint32_t slotid) const {return nonadj_const_iterator();}
		inline nonadj_const_iterator cend(uint32_t slotid) const {return nonadj_const_iterator();}

		// Iterate over typed 

		template<typename T>
		using data_iterator = Iterator<TypedBlock<T>, false>;
		template<typename T>
		using data_const_iterator = Iterator<const TypedBlock<T>, false>;

		template<typename T>
		inline data_iterator<T> begin(uint32_t slotid) {return data_iterator<T>(this->get(slotid));}
		template<typename T>
		inline data_const_iterator<T> begin(uint32_t slotid) const {return data_const_iterator<T>(this->get(slotid));}
		template<typename T>
		inline data_const_iterator<T> cbegin(uint32_t slotid) const {return data_const_iterator<T>(this->get(slotid));}

		template<typename T>
		inline data_iterator<T> end(uint32_t slotid) {return data_iterator<T>();}
		template<typename T>
		inline data_const_iterator<T> end(uint32_t slotid) const {return data_const_iterator<T>();}
		template<typename T>
		inline data_const_iterator<T> cend(uint32_t slotid) const {return data_const_iterator<T>();}

	private:
		// INTERNAL HELPERS

		// Ensure there's an empty block of datasize at least new_alloc_space after the specified block (block->adjacent())
		bool make_space_after(Block& containing_block, uint32_t new_alloc_space) {
			if (new_alloc_space % 4) new_alloc_space += 4 - (new_alloc_space % 4);
			while (containing_block.adjacent()->slotid != Block::SlotEmpty || containing_block.adjacent()->datasize < new_alloc_space) {
				// Operates as a loop:
				// 	- find empty block
				// 	- shift prior contents into it by units of 4
				if (containing_block.adjacent()->slotid == Block::SlotEnd) return false;
				Block* next_empty = containing_block.adjacent()->slotid != Block::SlotEmpty ? containing_block.adjacent() : containing_block.adjacent()->adjacent();
				Block* move_region_start = next_empty;
				while (*next_empty) {
					next_empty = next_empty->adjacent();
				}
				if (next_empty->slotid == Block::SlotEnd) return false;

				// Now we have the following situation:
				// | cb | xxxx | e |
				// where cb is containing_block and e is an empty block.
				//
				// We want to swap cb->adjacent() with the empty block, effectively (or insert it, etc.)
				//
				// The total space we can reclaim is new_alloc_space, minus the header.
				// If cb->adjacent() is empty, the space we have to reclaim is new_alloc_space - cb->adjacent()->datasize
				// otherwise, we have to reclaim the entire new_alloc_space + 4.
				//
				// The total space we can reclaim from e = 4 + its size.
				// 
				// If the remaining space in e is more than 4 (rounded to the nearest 4), we shrink the datasize, otherwise we delete the chunk entirely.
				// After reclaiming we memmove

				uint32_t remaining_new_alloc_space = containing_block.adjacent() ? new_alloc_space + 4 : (new_alloc_space - containing_block.adjacent()->datasize);
				if (next_empty->datasize % 4) next_empty->datasize += (4 - next_empty->datasize % 4);
				uint32_t reclaimable_space = next_empty->datasize + 4;
				if (reclaimable_space % 4) {
					reclaimable_space += 4 - (reclaimable_space % 4);
				}

				uint32_t total_reclaimed = 0;

				// If we reclaim the entire thing...
				if (reclaimable_space <= remaining_new_alloc_space) {
					// Just do a boring memmove
					memmove(((uint8_t *)move_region_start) + reclaimable_space, move_region_start, reinterpret_cast<uintptr_t>(next_empty) - reinterpret_cast<uintptr_t>(move_region_start));
					total_reclaimed = reclaimable_space;
				}
				else {
					// Be slightly more intelligent, do the same as above but write a new block at the end.
					Block old_empty = *next_empty;
					old_empty.datasize -= remaining_new_alloc_space;
					// TODO assert old_empty.datasize > remaining_new_alloc_space

					memmove(((uint8_t *)move_region_start) + remaining_new_alloc_space, move_region_start, reinterpret_cast<uintptr_t>(next_empty) - reinterpret_cast<uintptr_t>(move_region_start));
					total_reclaimed = remaining_new_alloc_space;
					*(next_empty + (remaining_new_alloc_space / 4)) = old_empty;
				}

				if (*containing_block.adjacent()) {
					// Replace it with a block
					*containing_block.adjacent() = Block{};
					containing_block.adjacent()->slotid = Block::SlotEmpty;
					containing_block.adjacent()->datasize = total_reclaimed - 4;
					containing_block.adjacent()->location = Block::LocationCanonical;
				}
				else {
					// Expand the size of the block
					containing_block.adjacent()->datasize += total_reclaimed;
				}
			} // this loop naturally ends at the right point.
			containing_block.adjacent()->shrink(new_alloc_space);
			return true;
		}

		// Split a block into two equally labelled but separate chunks
		bool split_block(Block& block, uint32_t offset_in_block) {
			// Compute and store length of new block
			uint32_t new_length = block.datasize - offset_in_block;
			// If the block is remote, do something very simple
			if (block.location == Block::LocationRemote) {
				if (!make_space_after(block, 0)) return false;
				block.shrink(offset_in_block);
				goto copy_data;
			}

			// Create an empty space after the block with enough space to fit the block's contents after offset_in_block + 4 bytes + alignment including the null space
			{
				uint32_t space_required = 4;
				if (offset_in_block % 4) {
					space_required += 4 - (offset_in_block % 4);
				}

				// Create that much space
				if (!make_space_after(block, space_required - 4)) return false;

				// Shift with memmove
				char * starting_pos = ((char *)block.data()) + offset_in_block;
				memmove(starting_pos + space_required, starting_pos, block.datasize - offset_in_block);

				// Shrink block to generate header in right place
				block.shrink(offset_in_block);
			}

copy_data:
			// Copy other header details.
			block.adjacent()->slotid = block.slotid;
			block.adjacent()->location = block.location;
			block.adjacent()->temperature = block.temperature;
			block.adjacent()->flags = block.flags;
			block.adjacent()->datasize = new_length;

			return true;
		}

		// Move a block to the left of another, moving the middle region wordwise
		void move_left(Block& to_move, Block& in_front_of) {
			if (in_front_of.adjacent() == &to_move) return;
			
			uint32_t * middle_start = reinterpret_cast<uint32_t *>(in_front_of.adjacent());
			uint32_t * middle_end = reinterpret_cast<uint32_t *>(&to_move);
			uint32_t * paste_from = middle_end;
			uint32_t * paste_to = middle_start;

			// Compute total length
			uint32_t paste_amount = to_move.adjacent() - &to_move;
			for (int i = 0; i < paste_amount; ++i) {
				uint32_t temp = *paste_from++;
				memmove(middle_start + 1, middle_start, (middle_end - middle_start) * 4);
				middle_start++;
				middle_end++;
				*paste_to++ = temp;
			}
		}
	};
}

#endif
