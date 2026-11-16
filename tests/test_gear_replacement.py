"""Exercise the real replacement helper with inventory failures (no game/database needed)."""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/PlayerbotMgr.cpp").read_text()
start = source.index("    bool EquipItemEntry(")
end = source.index("    // Autogear", start)
helper = source[start:end]
harness = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
using uint8=uint8_t; using uint16=uint16_t; using uint32=uint32_t;
constexpr uint8 NULL_BAG=255, NULL_SLOT=255;
constexpr int EQUIP_ERR_OK=0;
using ItemPosCountVec=std::vector<int>;
struct Item {
 uint32 entry; uint16 pos;
 uint32 GetEntry() const { return entry; }
 uint16 GetPos() const { return pos; }
 uint8 GetBagSlot() const { return pos>>8; }
 uint8 GetSlot() const { return pos&255; }
};
struct Player {
 std::map<uint16,Item*> items;
 Item old{10,5}, created{20,30};
 bool equipAllowed=true, storageAllowed=true, creationAllowed=true, swapAllowed=true;
 int destroyed=0;
 Player() { items[5]=&old; }
 int CanEquipNewItem(uint8 slot,uint16& dest,uint32,bool) { dest=slot; return equipAllowed?0:1; }
 Item* GetItemByPos(uint16 pos) { return items.count(pos)?items[pos]:nullptr; }
 int CanStoreNewItem(uint8,uint8,ItemPosCountVec&,uint32,uint32) { return storageAllowed?0:1; }
 Item* StoreNewItem(ItemPosCountVec const&,uint32 entry,bool) {
  if(!creationAllowed) return nullptr;
  created.entry=entry; items[30]=&created; return &created;
 }
 Item* EquipNewItem(uint16 dest,uint32 entry,bool) {
  if(!creationAllowed) return nullptr;
  created.entry=entry; created.pos=dest; items[dest]=&created; return &created;
 }
 void SwapItem(uint16 a,uint16 b) {
  if(!swapAllowed) return;
  auto first=GetItemByPos(a),second=GetItemByPos(b);
  items[a]=second;items[b]=first;
  if(first) first->pos=b;
  if(second) second->pos=a;
 }
 void DestroyItem(uint8 bag,uint8 slot,bool) {
  uint16 pos=(uint16(bag)<<8)|slot;
  assert(GetItemByPos(pos)!=&old); items.erase(pos);++destroyed;
 }
};
'''
harness += helper + r'''
int main() {
 { Player p; assert(!EquipItemEntry(&p,0,5)); assert(p.GetItemByPos(5)==&p.old); }
 { Player p; p.equipAllowed=false; assert(!EquipItemEntry(&p,20,5)); assert(p.GetItemByPos(5)==&p.old); }
 { Player p; p.storageAllowed=false; assert(!EquipItemEntry(&p,20,5)); assert(p.GetItemByPos(5)==&p.old); }
 { Player p; p.creationAllowed=false; assert(!EquipItemEntry(&p,20,5)); assert(p.GetItemByPos(5)==&p.old); }
 { Player p; p.swapAllowed=false; assert(!EquipItemEntry(&p,20,5)); assert(p.GetItemByPos(5)==&p.old); assert(p.destroyed==1); }
 { Player p; assert(EquipItemEntry(&p,20,5)); assert(p.GetItemByPos(5)==&p.created); assert(p.GetItemByPos(30)==&p.old); assert(p.destroyed==0); }
 { Player p; assert(EquipItemEntry(&p,10,5)); assert(p.GetItemByPos(5)==&p.old); assert(p.destroyed==0); }
 { Player p; p.items.clear(); assert(EquipItemEntry(&p,20,5)); assert(p.GetItemByPos(5)==&p.created); }
}
'''
compiler = shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("A C++ compiler (g++ or clang++) is required")
with tempfile.TemporaryDirectory(prefix="playerbot-gear-") as directory:
    root = Path(directory)
    cpp = root / "gear.cpp"
    binary = root / "gear.exe"
    cpp.write_text(harness)
    subprocess.run([compiler, "-std=c++17", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: eight replacement scenarios preserve existing gear and handle failure safely")
