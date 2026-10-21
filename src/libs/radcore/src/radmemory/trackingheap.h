#ifndef TRACKINGHEAP_H_
#define TRACKINGHEAP_H_
//-----------------------------------------------------------------------------
// Copyright (C) 2001 Radical Entertainment Ltd.  All rights reserved.
//
// staticheap.h
//
// Description: a heap that does nothing but track memory allocations
//
// Modification History:
//
//  + Created Mar 21, 2003 Ian Gipson
//-----------------------------------------------------------------------------

#include <radmemory.hpp>
#include "platalloc.hpp"
#include <climits>
#include <map>
#include <mutex>
#include <new>

template< class T >
class TrackingHeapMapAllocator
{
public:
    typedef T value_type;

    TrackingHeapMapAllocator() noexcept = default;

    template< class U >
    TrackingHeapMapAllocator( const TrackingHeapMapAllocator< U >& ) noexcept
    {
    }

    T* allocate( size_t count )
    {
        if( count > static_cast< size_t >( UINT_MAX ) / sizeof( T ) )
        {
            throw std::bad_alloc();
        }

        void* memory = radMemoryPlatAlloc( static_cast< unsigned int >( count * sizeof( T ) ) );
        if( memory == NULL )
        {
            throw std::bad_alloc();
        }
        return static_cast< T* >( memory );
    }

    void deallocate( T* memory, size_t ) noexcept
    {
        radMemoryPlatFree( memory );
    }
};

template< class T, class U >
bool operator==( const TrackingHeapMapAllocator< T >&, const TrackingHeapMapAllocator< U >& ) noexcept
{
    return true;
}

template< class T, class U >
bool operator!=( const TrackingHeapMapAllocator< T >&, const TrackingHeapMapAllocator< U >& ) noexcept
{
    return false;
}

//-----------------------------------------------------------------------------
// Definitions
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// StaticHeap
//-----------------------------------------------------------------------------
class TrackingHeap:
	public IRadMemoryHeap,
    public radRefCount
{
public:
    TrackingHeap();
    ~TrackingHeap();
    void   AddRef( void );
    bool   CanFreeMemory( void* pMemory );
    bool   CanFreeMemoryAligned( void* pMemory );
    void   FreeMemory( void* pMemory );
    void   FreeMemoryAligned( void* pMemory );
    void*  GetMemory ( unsigned int size );
    void*  GetMemoryAligned( unsigned int size, unsigned int align );
    void   GetMemoryObject( IRadMemoryObject** ppMemoryObject, unsigned int size );
	void   GetMemoryObjectAligned( IRadMemoryObject ** ppIRadMemoryObject, unsigned int size, unsigned int alignment );
    unsigned int GetSize();
    void   GetStatus(
		unsigned int* totalFreeMemory,
		unsigned int* largestBlock,
		unsigned int* numberOfObjects,
		unsigned int* highWaterMark );
    void   Release( void );
    void   SetSize( size_t size );
    void   TrackAllocations( bool trackAllocations );
    bool   ValidateHeap( void );
protected:
    void   RecordAllocation( void* address, size_t size );
    typedef std::pair< void* const, size_t > ADDRESS_SIZE_PAIR;
    typedef std::map< void*, size_t, std::less< void* >, TrackingHeapMapAllocator< ADDRESS_SIZE_PAIR > > ADDRESS_SIZE_MAP;
    std::mutex m_Mutex;
    ADDRESS_SIZE_MAP m_Map;
    bool   m_TrackAllocations;
    size_t m_TotalAllocated;
    size_t m_MaxSize;
    size_t m_HighWater;
    unsigned int m_NumberOfAllocations;
};

IRadMemoryHeap* radMemoryCreateTrackingHeap( unsigned int size,
	radMemoryAllocator allocator,
    const char * pName );

#endif // TRACKINGHEAP_H_

