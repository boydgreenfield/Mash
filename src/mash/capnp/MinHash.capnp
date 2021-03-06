using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("capnp");

@0xc4c8b1ada05e7704;

struct MinHash
{
	struct ReferenceList
	{
		struct Reference
		{
			sequence @0 : Text;
			quality @1 : Text;
			length @ 2 : UInt32;
			name @3 : Text;
			comment @4 : Text;
			hashes32 @5 : List(UInt32);
			hashes64 @6 : List(UInt64);
		}
		
		references @0 : List(Reference);
	}
	
	struct LocusList
	{
		struct Locus
		{
			sequence @0 : UInt32;
			position @1 : UInt32;
			hash32 @2 : UInt32;
			hash64 @3 : UInt64;
		}
		
		loci @0 : List(Locus);
	}
	
	kmerSize @0 : UInt32;
	windowSize @1 : UInt32;
	minHashesPerWindow @2 : UInt32;
	concatenated @3 : Bool;
	error @6 : Float32;
	noncanonical @7 : Bool;
	
	referenceList @4 : ReferenceList;
	locusList @5 : LocusList;
}
