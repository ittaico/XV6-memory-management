#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}
 //***TOCHANGE***

pte_t* walkpgdir2(pde_t *pgdir, const void *va){
  pde_t *pde;
  pte_t *pgtab;
  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
    return &pgtab[PTX(va)];
  } else return 0;
}



// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.


static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}


// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }

    #ifndef NONE
      if(myproc()->pim > MAX_PSYC_PAGES){
        panic("memory full");
      }
      if(myproc()->pim == MAX_PSYC_PAGES){
        int swapFileIndex = pageSelector(myproc());
        swapAndWrite(swapFileIndex, myproc());
      }
      updatePages((void*) a, mem, myproc());
    #endif
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;


  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
       #ifndef NONE
        if(myproc()->pgdir == pgdir){
          removePageAndUpdate((void*) a, myproc());
        }
      #endif
    }
  #ifndef NONE
      //Checking if the paged out to secondary storage
     else if (*pte & PTE_PG && myproc()->pgdir == pgdir) {
      int i;
      for (i = 0; i < MAX_PSYC_PAGES; i++) {
        if (myproc()->sd[i].va == (char*)a)
          break;
      }
      if (i == MAX_PSYC_PAGES || myproc()->sd[i].inSF == 0)
          panic("error - deallocuvm fuinction - Paged not out to secondary storage");
      myproc()->sd[i].inSF = 0;     
      myproc()->sp--;
      *pte = 0;
    }
   #endif
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  pte_t * pte2level;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    if(*pte & PTE_PG){
      pte2level = walkpgdir(d, (void *) i, 1);
      flags = PTE_FLAGS(*pte);                         //copy the parent flags
      *pte2level = PTE_U | PTE_PG | PTE_W | PTE_ADDR(*pte) | (int) flags;     //update the flags
      *pte2level = *pte2level & ~PTE_P;              //clear the PTE_P flag
      continue;
    }
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

// writing to the swap file
void
swapAndWrite(int pageNum, struct  proc *p){
  uint location;
  int count,i;
  struct sDet *sd;
  pte_t *pte = walkpgdir(p->pgdir, p->pd[pageNum].va,0);
  if(!*pte){
    panic("error - no page table entry");
  }
  else{
    for(sd = p->sd,count = 0; sd < &p->sd[MAX_PSYC_PAGES];sd++){
      if(!sd->inSF){
        break;
      }
      count++;
    }
    if (sd >= &p->sd[MAX_PSYC_PAGES]){
      panic("Swap File is Full");
    }
    location = count*PGSIZE;
    int qPGSIZE = PGSIZE/4;
    for (i=0; i<4; i++){
      writeToSwapFile(p,p->pd[pageNum].page + (i * qPGSIZE), location + (i * qPGSIZE), qPGSIZE);  //writeToSwapFile(proc *p,char * buffer,uint fileOffset,uint size)
    }
    sd->va = p->pd[count].va;     //Update the virtual address
    sd->inSF = 1;                 //Update the InSwapFile flag
    kfree(p->pd[count].page);     //Free the page from the memory
    removePageAndUpdate(p->pd[count].va,p);  //*************************
    p->sp++;            //increase the Swap Page counter of the process
    p->ts++;            //increase the Total Swap Page counter of the process
    *pte = (*pte | PTE_PG) & ~PTE_P;      //**************************
    lcr3(V2P(p->pgdir));            // By using the LCR3 rgister and the V2P funcation we update the Page Directory 
  }
}


// Returns the index of the page to to be removed, according to the different page replacement scheme.
int
pageSelector(struct proc *p){
  int ans = -1;
  // NFU + AGING
  // Find the page that is 'accCount' var is the lowest. 
  #ifdef NFUA
    uint lowest = 0xffffffff;
    for(int i = 0 ; i < MAX_PSYC_PAGES ; i++){
      if(!p->pd[i].inMem ){
        panic("error - page is not in memory");
      }
      pte_t* pte = walkpgdir2(p->pgdir, p->pd[i].va);
      if(!(*pte & PTE_U)){
        continue;
      }
      if(p->pd[i].accCount <= lowest){
        lowest = p->pd[i].accCount;
        ans = i;
      }
    }
  #endif

  // Leaset accesed page + AGING
  // Find the page with the lowest 1's in 'accCount',
  // if the number of 1 in 'accCount' is equale, then take the the lowest 'accCount'
  #ifdef LAPA
  uint minNumberOf1 = 33;
  uint lowest = 0xffffffff;
  for(int i = 0 ; i < MAX_PSYC_PAGES ; i++){
    if(!p->pd[i].inMem){
      panic("error - page not in memory");
    }
    pte_t* pte = walkpgdir2(p->pgdir, p->pd[i].va);
    if(!(*pte & PTE_U)){
      continue;
    }
    uint currentaccCount = p->pd[i].accCount;
    int countNumOf1 = 0;
    while(currentaccCount) {
        countNumOf1 += currentaccCount % 2;   
        currentaccCount >>= 1;
    }

    if(countNumOf1 < minNumberOf1 || (countNumOf1 == minNumberOf1 && p->pd[i].accCount <= lowest)){
      lowest = p->pd[i].accCount;
      minNumberOf1 = countNumOf1;
      ans = i;
    }
  }
  #endif

  // Second chance FIFO
  // Find the first page (FIFO) that its PTE_A is 0.
  #ifdef SCFIFO
    int accessed = 1;

    //loop unntil we find a page which wan't accessed.
    while(accessed) {
      if(!p->pd[p->head].inMem){
        panic("error - page not in memory");
      }
      pte_t* pte = walkpgdir2(p->pgdir, p->pd[p->head].va);
      if(!(*pte & PTE_U)){
        p->head = (p->head + 1) % MAX_PSYC_PAGES;
        continue;
      }
      accessed = *pte & PTE_A;                      
      *pte = *pte & ~PTE_A;
      p->head = (p->head + 1) % MAX_PSYC_PAGES;
    }
    //return the first page that wasn't accessed (we go back -1 because of the loop) 
    ans = (p->head + MAX_PSYC_PAGES - 1) % MAX_PSYC_PAGES;
  #endif

  // Advancing Queue
  #ifdef AQ
    int i;
    for(i = 0 ; i < MAX_PSYC_PAGES ; i++){
      if(!p->pd[i].inMem){
        panic("error - page not in memory");
      }
    }

    for(i = 0 ; i < MAX_PSYC_PAGES ; i++){
      pte_t* pte = walkpgdir2(p->pgdir, p->pd[i].va);
      if((*pte & PTE_U)){
        break;
      }
    }
    ans = i;
  #endif


  if((ans < 0)||ans >= MAX_PSYC_PAGES){
    panic("error - pageSelector end function - page limit violation");
  }
  return ans;
}

//The function will get the virtual address and the process and will remove 
//page from the struct, and update the relevnt vars in the process struct
void
removePageAndUpdate(void *va,struct proc *p){
  int i;   
  //Case for NFUA/LAPA/SCFIFO
   #ifndef AQ
    for(i = 0; i < MAX_PSYC_PAGES; i++)
      if(p->pd[i].va == va)
        break;
    if(i == MAX_PSYC_PAGES)
      return;
    if (p->pd[i].inMem == 1){
      p->pd[i].inMem = 0;
      p->pd[i].va = 0;
      p->pim--;
    }
  #endif  


  #ifdef AQ
    for(i = 0 ; i < MAX_PSYC_PAGES ; i++){
      if(p->pd[i].va == va){
        break;
      }
    }
    if (i >= p->pim)
        panic("removePageAndUpdate function - index is illegal");
    while(i < p->pim - 1){
      p->pd[i].va = p->pd[i+1].va;
      p->pd[i].page = p->pd[i+1].page;
      if (p->pd[i].inMem == 0 || p->pd[i+1].inMem == 0)
          panic("error - page not in memory");
      i++;
    } 
    //Remove the last page in the memory
    if (p->pd[p->pim - 1].inMem){
      p->pd[p->pim - 1].inMem = 0;
      p->pd[p->pim - 1].va = 0;
      p->pim--;
    } else panic("error - page not in memory");
  #endif
  }

  /*
    #ifdef NFUA
    for(i = 0; i < MAX_PSYC_PAGES; i++)
      if(p->pd[i].va == va)
        break;
    if(i == MAX_PSYC_PAGES)
      return;
    if (p->pd[i].inMem == 1){
      p->pd[i].inMem = 0;
      p->pd[i].va = 0;
      p->pim--;
    }
  #endif


  #ifdef LAPA
    int i;
    for(i = 0; i < MAX_PSYC_PAGES; i++){
      if(p->pages[i].va == va){
        break;
      }
    }
    if(i == MAX_PSYC_PAGES){
      return;
    }
    if (p->pages[i].isInMemory == 1){
      p->pages[i].isInMemory = 0;
      p->pages[i].va = 0;
      p->pagesInMem--;
    }
  #endif

  #ifdef SCFIFO
    int i;
    for(i = 0; i < MAX_PSYC_PAGES; i++){
      if(p->pages[i].va == va){
        break;
      }
    }
    if(i == MAX_PSYC_PAGES){
      return;
    }
    if (p->pages[i].isInMemory == 1){
      p->pages[i].isInMemory = 0;
      p->pages[i].va = 0;
      p->pagesInMem--;
    }
  #endif

  #ifdef AQ
    int i;
    for(i = 0 ; i < MAX_PSYC_PAGES ; i++){
      if(p->pages[i].va == va){
        break;
      }
    }
    if (i >= p->pagesInMem)
        panic("index is illegal");
    while(i < p->pagesInMem - 1){
      p->pages[i].va = p->pages[i+1].va;
      p->pages[i].pPage = p->pages[i+1].pPage;
      if (p->pages[i].isInMemory == 0 || p->pages[i+1].isInMemory == 0)
          panic("error");
      i++;
    } 
    if (p->pages[p->pagesInMem - 1].isInMemory){
      p->pages[p->pagesInMem - 1].isInMemory = 0;
      p->pages[p->pagesInMem - 1].va = 0;
      p->pagesInMem--;
    } else panic("error");
  #endif
  
}
*/
//The function will update the pages after read and at the allocuvm
void
updatePages(void *va,void *page,struct proc *p){
  int i;
  for(i = 0; i < MAX_PSYC_PAGES; i++){
    if(p->pd[i].inMem == 0){
      break;
    }
  }
  if(i == MAX_PSYC_PAGES){
    panic("function updatePages - memory is full");
  }

  //NFUA
  #ifdef NFUA
    p->pd[i].accCount = 0;
  #endif

  //LAPA
  #ifdef LAPA
    p->pd[i].accCount = 0xffffffff;
  #endif
  p->pd[i].inMem = 1;
  p->pd[i].va = va;
  p->pd[i].page = page;
  p->pim++;
}

void
swapAndRead(void *va,struct proc *p){
  struct sDet* sd;
  int i;
  cprintf("the VA is:%d \n",va);
  for(sd = p->sd,i = 0; sd < &p->sd[MAX_PSYC_PAGES] ; sd++,i++){
      cprintf("the sd->VA is:%d \n",sd->va);
    if(sd->inSF && sd->va == va){
      break;
    }
  }
  if(sd >= &p->sd[MAX_PSYC_PAGES]){
    panic("error - swapAndRead function -sd MAX");
  }
  pte_t* pte = walkpgdir(p->pgdir, va, 1);
  if(!*pte){
    panic("error - swapAndRead function - Not pte");
  }
  char* newPage = kalloc();
  uint location = i * PGSIZE;
  int qPGSIZE = PGSIZE/4;
  for(i = 0 ; i < 4 ; i++){
    readFromSwapFile(p, newPage + (i * qPGSIZE), location + (i * qPGSIZE), qPGSIZE);
  }
  *pte = (V2P(newPage) | PTE_P | PTE_U | PTE_W) & ~PTE_PG;
  sd->inSF = 0;
  updatePages(va, newPage, p);
  p->sp--;
  lcr3(V2P(p->pgdir));
}



//PAGEBREAK!
// Blank page.             
//PAGEBREAK!              
// Blank page.     
//PAGEBREAK!       
// Blank page.  
