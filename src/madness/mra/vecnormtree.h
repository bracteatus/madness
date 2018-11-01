#ifndef MAD_VEC_NORM_TREE_H
#define MAD_VEC_NORM_TREE_H

#include <iostream>
#include <type_traits>
#include <madness/world/MADworld.h>
#include <madness/world/print.h>
#include <madness/mra/key.h>
#include <array>
#include <tuple>

/*


  Given vector of (same spin) occupied orbitals phi[j] we want to apply exchange operator 
  to vector of target orbitals f[i]

  r[i] = sum(j) phi[j] * (G phi[j] f[i])

  where G is the Coulomb Green's function.

  Want to use sparsity (localization) of phi[j] in two places

  - finite support of phi[j]*f[i] --- do this with vmulspase and truncation

  - only need potential of phi[j]*f[i] where phi[j] is significant ... so when applying 
    the integral operator to an input box (n,l) we need the norm of phi[j] in output 
    box (n,l') in order to compute the screening.

  To make this tractable assume that we will only need displacements up to 1 in any direction, so l' = l-1,l,l+1

  Thus, first compute norm trees for each orbital phi[j] ... have this code ... so now know the norm of phi[j] in each box.

  Next, walk down the union of the trees of all phi, collect all of the norms into a vector,
  and pass the vector to all interested neighbors (in 3D there will be 27).

  If we are below (at a finer level than) the support of a given orbital can either uniformly 
  distribute the norm of a parent box to all children or just use the entire value of the parent 
  in each child. Thus, if we hit a leaf node we need to walk data down the tree as well as pass 
  to neighbors.  Might as well always do this.

  When applying operator can limit l' appropriately, look up norm info --- if we don't have it then can 
  either search up the tree to find it or, assuming this is just a few boxes, we can just always compute.


 */

namespace madness {

  /// Constructs and holds an estimate of the norm of each function in the 27 boxes usually touched by an integral operator.  This is to support exchange.
  template <std::size_t NDIM>
    class VectorNormTree : public WorldObject<VectorNormTree<NDIM>> {
    //static_assert(NDIM==3);
  public:
    typedef Key<NDIM> keyT;
    typedef double normT;
    static const size_t NVEC=27;
    typedef std::array< std::vector<normT>, NVEC > arrayT;

    //private:

    struct valueT {
      bool has_children;
      arrayT v;

      template <typename Archive> void serialize(Archive& ar) {ar & has_children & v;}
    };

    typedef WorldContainer<keyT,valueT> dcT;
  
    dcT values;

    const size_t nfunc;

    // Given neighbor i,j,k in -1,0,1 returns index of box [0,26]
    static constexpr int index(int i, int j, int k) {
      return (i+1)*9 + (j+1)*3 + (k+1);
    }

    // Given offset of child in parent and index of child neighbor (i in -1,0,1) returns index of containing neighbor of parent
    static int child_to_parent(int offset, int i) {
      int sum = i + offset;
      if (sum==0 || sum==1) return 0;
      else if (sum==2) return 1;
      else return -1;
    }

    const int MIDDLE = index(0,0,0); // index of center box

    // Make list of keys in container so that we can iterate while mutating the container
    std::vector<keyT> make_key_list() const {
      std::vector<keyT> r;
      r.reserve(values.size());
      for (auto& it : values) r.push_back(it.first);
      return r;
    }
	
    // Child invokes this method in parent to inform it of birth
    void you_have_a_child(const keyT& key) {
      typename dcT::accessor acc;
      bool newnode = values.insert(acc,key);
      //MADNESS_ASSERT(!newnode);
      if(newnode){
           for(auto& vi : acc->second.v) {
               vi.resize(nfunc);
               for(auto& x : vi) x = -1.0; // -ve value so can identify missing data
           }
           auto pkey = acc->first.parent();
           this->send(values.owner(pkey), &VectorNormTree<NDIM>::you_have_a_child, pkey); 
      }
      acc->second.has_children = true;  // maybe not all children exist
    }

    // Invoked by putv to set value from neighbor specified by index ... question about what we do with has_children
    void setv(const keyT& key, int index, const std::vector<normT>& norms, bool has_children) {
      MADNESS_ASSERT(index>=0 && index<int(NVEC));
      typename dcT::accessor acc;
      bool newnode = values.insert(acc,key);
      if (newnode) {
	for (auto& vi : acc->second.v)  {
	  vi.resize(nfunc);
	  for (auto& x : vi) x = -1.0; // -ve value so can identify missing data
	}
	acc->second.has_children = false;
     auto pkey = acc->first.parent();
     this->send(values.owner(pkey), &VectorNormTree<NDIM>::you_have_a_child, pkey); 
      }
      //auto n = key.level();
      //auto l = key.translation();
      //if (n==1 && l[0]==1 && l[1]==0 && l[2]==1) ::madness::print("in setv: ", key, index, norms);

      acc->second.v[index] = norms;
    }

    // Ensures connection to parent exists for all boxes
    void connect_to_parent() {
      for (auto& it : values) {
	const bool has_children = it.second.has_children;	
	  const auto& key = it.first;
	  if (!has_children){
       //if(key.level() > 0){
	  auto parent = key.parent();
	  this->send(values.owner(parent), &VectorNormTree<NDIM>::you_have_a_child, parent);
	  }
      }
      World& world = this->get_world();
      world.gop.fence();
    }

    // Call by putv to propagte data down the tree --- tell children what knowledge parent has about their neighbors
    // pvalue is data from parent
    void walk_down(const keyT& key, const valueT& pvalue) {
      typename dcT::accessor acc;
      bool newnode = values.insert(acc,key);
      valueT& value = acc->second;
      if (newnode) {
	for (auto& vi : value.v)  {
	  vi.resize(nfunc);
	  for (auto& x : vi) x = -1.0; // -ve value so can identify missing data
	}
	value.has_children = false;
      }


      if (key.level() > 0) {
	const auto& l = key.translation();
	const auto& p = key.parent().translation();
	Level n = key.level();
	Translation maxl = Translation(1)<<n;
	int offi = l[0]-2*p[0]; // 0 or 1 for left or right
	int offj = l[1]-2*p[1];
	int offk = l[2]-2*p[2];
	
	for (int i=-1; i<=1; i++) {
	  for (int j=-1; j<=1; j++) {
	    for (int k=-1; k<=1; k++) {
	      int index = this->index(i,j,k);
	      for (size_t f=0; f<nfunc; f++) {
		if (value.v[index][f] == -1.0) { // if data in me has not been set

            //ignore this section if the data we're missing is in a boundary box
		  if (l[0]+i<0 || l[0]+i >= maxl) continue;
		  if (l[1]+j<0 || l[1]+j >= maxl) continue;
		  if (l[2]+k<0 || l[2]+k >= maxl) continue;


		  int pi = child_to_parent(offi, i);
		  int pj = child_to_parent(offj, j);
		  int pk = child_to_parent(offk, k);
		  int pindex = this->index(pi, pj, pk);
		  value.v[index][f] = pvalue.v[pindex][f];
		}
	      }
	    }      
	  }
	}
      }

      if (value.has_children) {
	for (KeyChildIterator<NDIM> kit(key); kit; ++kit) {
	  const keyT& child = kit.key();
	  this->send(values.owner(child), &VectorNormTree<NDIM>::walk_down, child, value);
	}
      }

    }


  public:

    VectorNormTree() = delete;
  
    VectorNormTree(World& world, 
		   std::shared_ptr<WorldDCPmapInterface<Key<NDIM> > > pmap,
		   size_t nfunc) 
      : WorldObject<VectorNormTree<NDIM>>(world)
      , values(world, pmap)
      , nfunc(nfunc)
    {this->process_pending();}

    // Invoked by method in func impl to initially insert norms of middle box (0,0,0) for each function as computed by norm_tree
    void set(size_t i, const keyT& key, double norm, bool has_children) {
      typename dcT::accessor acc;
      bool newnode = values.insert(acc,key);
      if (newnode) {
	for (auto& vi : acc->second.v)  {
	  vi.resize(nfunc);
	  for (auto& x : vi) x = -1.0; // -ve value so can identify missing data
	}
      }
      MADNESS_ASSERT(i<nfunc);
      acc->second.v[MIDDLE][i] = norm;
      acc->second.has_children = (acc->second.has_children || has_children);
    }

    void putv() {
      World& world = this->get_world();
      world.gop.fence(); // required unless fence in funcinp::make_vec_norm_tree
      auto keys = make_key_list();
      world.gop.fence();

      // Communicate values to neighbors
      for (const auto& key : keys) {
	typename dcT::accessor acc;
	bool newnode = values.insert(acc,key);
	MADNESS_ASSERT(!newnode);

	Level n = key.level();
	Translation maxl = Translation(1)<<n;
	const auto& lold = key.translation();
	for (int i=-1; i<=1; i++) {
	  for (int j=-1; j<=1; j++) {
	    for (int k=-1; k<=1; k++) {
	      if (i || j || k) {
		auto l = lold;
		l[0] += i; if (l[0]<0 || l[0] >= maxl) continue;
		l[1] += j; if (l[1]<0 || l[1] >= maxl) continue;
		l[2] += k; if (l[2]<0 || l[2] >= maxl) continue;
		keyT neigh(n,l);
		const auto& norms = acc->second.v[MIDDLE];
		const bool has_children = acc->second.has_children;
		this->send(values.owner(neigh), &VectorNormTree<NDIM>::setv, neigh, index(-i, -j, -k), norms, has_children);
	      }
	    }
	  }
	}
      }
      world.gop.fence();
      //connect_to_parent();

      // ::madness::print("BEFORE WALKDOWN");

      // for (int i=-1; i<=1; i++) {
      // 	for (int j=-1; j<=1; j++) {
      // 	  for (int k=-1; k<=1; k++) {
      // 	    ::madness::print(i,j,k,index(i,j,k));
      // 	  }
      // 	}
      // }

      // print();
      // ::madness::print("END OF BEFORE WALKDOWN");

      // Propagate values down the tree to fill in gaps where possible
      keyT root(0);
      if (world.rank() == values.owner(root)) {
	this->send(values.owner(root), &VectorNormTree<NDIM>::walk_down, root, valueT());
      }
      world.gop.fence();

      // Verify
      for (auto& it : values) {
	const auto& key = it.first;
	const auto& value = it.second;
	const auto& lold = key.translation();
	Level n = key.level();
	Translation maxl = Translation(1)<<n;
	for (int i=-1; i<=1; i++) {
	  for (int j=-1; j<=1; j++) {
	    for (int k=-1; k<=1; k++) {
	      auto l = lold;
	      l[0] += i; if (l[0]<0 || l[0] >= maxl) continue;
	      l[1] += j; if (l[1]<0 || l[1] >= maxl) continue;
	      l[2] += k; if (l[2]<0 || l[2] >= maxl) continue;
	      auto index = this->index(i,j,k);
	      for (auto f : value.v[index]) {
		if (f == -1.0) ::madness::print("missing? ", key, i, j, k);
	      }
	    }
	  }
	}
      }
      world.gop.fence();
    }

    void print() const {
      for (auto& it : values) {
	const auto& key = it.first;
	const valueT& value = it.second;
	std::cout << "\n" << key << " : " << value.has_children << std::endl;
	for (size_t i=0; i<nfunc; i++) {
	  std::cout << "    " << i << " : ";
	  for (size_t neigh=0; neigh<NVEC; neigh++) {
	    MADNESS_ASSERT(value.v[neigh].size() == nfunc);
         if(neigh % 3 == 0) std::cout << std::endl;
	    std::cout << value.v[neigh][i] << " ";
	  }
	  std::cout << std::endl;
	}  
      }
    }

    template <typename Archive> void serialize(Archive& ar) {throw "cannot serialize vtree";}


  };
}
#endif
