package
{
   import Components.ImageFixture;
   import Shared.AS3.Data.BSUIDataManager;
   import Shared.AS3.Data.FromClientDataEvent;
   import Shared.AS3.Events.CustomEvent;
   import Shared.AS3.IMenu;
   import Shared.GlobalFunc;
   import flash.display.MovieClip;
   import flash.events.Event;
   import flash.events.KeyboardEvent;
   import flash.ui.Keyboard;
   
   [Embed(source="/_assets/assets.swf", symbol="symbol117")]
   public class FavoritesMenu extends IMenu
   {
      
      public static const FS_LEFT_3:uint = 0;
      
      public static const FS_LEFT_2:uint = 1;
      
      public static const FS_LEFT_1:uint = 2;
      
      public static const FS_RIGHT_1:uint = 3;
      
      public static const FS_RIGHT_2:uint = 4;
      
      public static const FS_RIGHT_3:uint = 5;
      
      public static const FS_UP_3:uint = 6;
      
      public static const FS_UP_2:uint = 7;
      
      public static const FS_UP_1:uint = 8;
      
      public static const FS_DOWN_1:uint = 9;
      
      public static const FS_DOWN_2:uint = 10;
      
      public static const FS_DOWN_3:uint = 11;
      
      public static const FS_NONE:uint = 12;
      
      public var CenterClip_mc:MovieClip;
      
      public var ItemInfo_mc:FavoriteAssigningInfoDisplay;
      
      public var AssignedItemIcon_mc:ImageFixture;
      
      public var Vignette_mc:MovieClip;
      
      public var SelectQuickslot_mc:MovieClip;
      
      public var Entry_0:FavoritesEntry;
      
      public var Entry_1:FavoritesEntry;
      
      public var Entry_2:FavoritesEntry;
      
      public var Entry_3:FavoritesEntry;
      
      public var Entry_4:FavoritesEntry;
      
      public var Entry_5:FavoritesEntry;
      
      public var Entry_6:FavoritesEntry;
      
      public var Entry_7:FavoritesEntry;
      
      public var Entry_8:FavoritesEntry;
      
      public var Entry_9:FavoritesEntry;
      
      public var Entry_10:FavoritesEntry;
      
      public var Entry_11:FavoritesEntry;
      
      public var FavoritesInfoA:Array;

      private var NativeFavoritesInfoA:Array;

      private var VirtualFavoritesInfoA:Array;

      public var FavoritesBanksActiveBank:int = 0;

      public var FavoritesBanksNativePage:Boolean = true;

      public var FavoritesBanksNativeSelection:Function = null;

      public var FavoritesBanksCaptureVisuals:Function = null;

      public var FavoritesBanksLog:Function = null;

      public var FavoritesBanksIconFrame:int = 2;

      public var FavoritesBanksClearSlot:Function = null;
      
      public var FavoritesBanksSwitchRow:Function = null;

      // 0 disables it. Flash key codes match Windows virtual key codes
      // for the keys this accepts, so the DLL passes the parsed INI
      // value straight through.
      public var FavoritesBanksClearSlotKey:int = 0;
      
      private var AssignedItem:Object = null;
      
      private var HasAssignedSlotOnce:Boolean = false;
      
      private var IsDataInitialized:Boolean = false;
      
      private var _SelectedIndex:uint = 12;
      
      // Which page the last render drew, or -1 before the first one.
      // The wheel opens with a slot already highlighted when the player
      // opened it with a D-pad direction: the engine sends that slot as
      // uStartingSelection and onDataUpdate applies it. Clearing the
      // selection on every render threw that away, so the direction that
      // opened the wheel stopped selecting anything. Only a real page
      // change clears it now.
      private var _RenderedBank:int = -1;
      
      public const TIMELINE_EVENT_CLOSE_ANIM_DONE:String = "onFinishedClosingAnim";
      
      private var OverEntry:Boolean = false;
      
      private const _UpDirectory:Array = [FS_UP_1,FS_UP_1,FS_UP_1,FS_UP_1,FS_UP_1,FS_UP_1,FS_UP_3,FS_UP_3,FS_UP_2,FS_UP_1,FS_DOWN_1,FS_DOWN_2,FS_UP_1];
      
      private const _DownDirectory:Array = [FS_DOWN_1,FS_DOWN_1,FS_DOWN_1,FS_DOWN_1,FS_DOWN_1,FS_DOWN_1,FS_UP_2,FS_UP_1,FS_DOWN_1,FS_DOWN_2,FS_DOWN_3,FS_DOWN_3,FS_DOWN_1];
      
      private const _LeftDirectory:Array = [FS_LEFT_3,FS_LEFT_3,FS_LEFT_2,FS_LEFT_1,FS_RIGHT_1,FS_RIGHT_2,FS_LEFT_1,FS_LEFT_1,FS_LEFT_1,FS_LEFT_1,FS_LEFT_1,FS_LEFT_1,FS_LEFT_1];
      
      private const _RightDirectory:Array = [FS_LEFT_2,FS_LEFT_1,FS_RIGHT_1,FS_RIGHT_2,FS_RIGHT_3,FS_RIGHT_3,FS_RIGHT_1,FS_RIGHT_1,FS_RIGHT_1,FS_RIGHT_1,FS_RIGHT_1,FS_RIGHT_1,FS_RIGHT_1];
      
      public function FavoritesMenu()
      {
         super();
         addFrameScript(12,this.frame13,17,this.frame18,20,this.frame21);
         addEventListener(KeyboardEvent.KEY_DOWN,this.onKeyDownHandler);
         addEventListener(KeyboardEvent.KEY_UP,this.onKeyUpHandler);
         addEventListener(FavoritesEntry.CLICK,this.SelectItem);
         addEventListener(FavoritesEntry.MOUSE_OVER,this.onFavEntryMouseover);
         addEventListener(FavoritesEntry.MOUSE_LEAVE,this.onFavEntryMouseleave);
         BSUIDataManager.Subscribe("FavoritesData",this.onDataUpdate);
         this.AssignedItemIcon_mc.mouseEnabled = false;
         this.AssignedItemIcon_mc.mouseChildren = false;
      }
      
      override public function onAddedToStage() : void
      {
         super.onAddedToStage();
         stage.focus = this;
      }
      
      private function onDataUpdate(param1:FromClientDataEvent) : void
      {
         this.NativeFavoritesInfoA = param1.data.aFavoriteItems;
         if(this.FavoritesBanksNativePage)
         {
            this.FavoritesInfoA = this.NativeFavoritesInfoA;
            if(this.FavoritesBanksCaptureVisuals != null && this.NativeFavoritesInfoA != null)
            {
               this.FavoritesBanksCaptureVisuals(this.NativeFavoritesInfoA);
            }
         }
         if(!this.IsDataInitialized)
         {
            this.assignedItem = param1.data.ItemToBeAssigned;
            this.selectedIndex = this.FavoritesBanksSwitchRow != null
               ? 0
               : param1.data.uStartingSelection;
            this.CenterClip_mc.gotoAndStop(this.isAssigningItem() ? "Inventory" : "Quick");
         }
         this.SelectQuickslot_mc.visible = this.isAssigningItem() && !this.HasAssignedSlotOnce;
         this.SelectQuickslot_mc.gotoAndPlay(this.SelectQuickslot_mc.visible ? "Open" : "Close");
         this.AssignedItemIcon_mc.visible = this.isAssigningItem() && !this.HasAssignedSlotOnce;
         this.AssignedItemIcon_mc.gotoAndPlay(this.AssignedItemIcon_mc.visible ? "Open" : "Close");
         this.RefreshFavoriteEntries();
         this.IsDataInitialized = true;
      }

      private function RefreshFavoriteEntries(param1:Boolean = false) : void
      {
         var _loc2_:uint = 0;
         var _loc3_:Object = null;
         var _loc4_:FavoritesEntry = null;
         while(_loc2_ < FS_NONE)
         {
            _loc3_ = this.FavoritesInfoA != null && _loc2_ < this.FavoritesInfoA.length ? this.FavoritesInfoA[_loc2_] : null;
            _loc4_ = this.GetEntryClip(_loc2_);
            if(_loc4_ != null)
            {
               _loc4_.LoadIcon(_loc3_,param1);
            }
            _loc2_++;
         }
         this.onSelectionChange();
      }

      public function FavoritesBanksRenderPage(param1:Array, param2:int, param3:Boolean) : void
      {
         this.FavoritesBanksActiveBank = param2;
         this.FavoritesBanksNativePage = param3;
         this.VirtualFavoritesInfoA = param1;
         if(param3 && this.NativeFavoritesInfoA != null)
         {
            this.FavoritesInfoA = this.NativeFavoritesInfoA;
         }
         else
         {
            this.FavoritesInfoA = this.VirtualFavoritesInfoA;
         }
         var _loc4_:Boolean = this._RenderedBank != -1 && this._RenderedBank != param2;
         if(_loc4_)
         {
            this.selectedIndex = FS_NONE;
         }
         this._RenderedBank = param2;
         this.RefreshFavoriteEntries(_loc4_);
      }
      
      public function isAssigningItem() : Boolean
      {
         return this.AssignedItem != null && this.AssignedItem.sName.length != 0;
      }
      
      public function get assignedItem() : Object
      {
         return this.AssignedItem;
      }
      
      public function set assignedItem(param1:Object) : void
      {
         this.AssignedItem = param1;
         if(this.isAssigningItem())
         {
            this.ItemInfo_mc.UpdateDisplay(this.AssignedItem);
            if(this.AssignedItem.iconImage.iFixtureType == ImageFixture.FT_SYMBOL)
            {
               this.AssignedItemIcon_mc.clipSizer = this.AssignedItem.bIsPower ? "Sizer_mc" : "";
               this.AssignedItemIcon_mc.centerClip = true;
            }
            else
            {
               this.AssignedItemIcon_mc.clipSizer = "";
               this.AssignedItemIcon_mc.centerClip = false;
            }
            this.AssignedItemIcon_mc.LoadImageFixtureFromUIData(this.AssignedItem.iconImage,"FavoritesIconBuffer");
            this.ItemInfo_mc.visible = true;
         }
      }
      
      public function get selectedIndex() : uint
      {
         return this._SelectedIndex;
      }
      
      public function set selectedIndex(param1:uint) : void
      {
         if(param1 != this._SelectedIndex)
         {
            if(this._SelectedIndex != FS_NONE)
            {
               this.GetEntryClip(this._SelectedIndex).selected = false;
            }
            this._SelectedIndex = param1;
            if(this._SelectedIndex != FS_NONE)
            {
               this.GetEntryClip(this._SelectedIndex).selected = true;
            }
            this.onSelectionChange();
            GlobalFunc.PlayMenuSound(this.selectionSound);
         }
      }
      
      public function get selectedEntry() : Object
      {
         return this.FavoritesInfoA != null && this._SelectedIndex >= 0 && this._SelectedIndex < this.FavoritesInfoA.length ? this.FavoritesInfoA[this._SelectedIndex] : null;
      }
      
      public function get selectionSound() : String
      {
         var _loc1_:String = "";
         switch(this.selectedIndex)
         {
            case FS_UP_1:
            case FS_DOWN_1:
            case FS_LEFT_1:
            case FS_RIGHT_1:
               _loc1_ = "UIMenuQuickUseFocusDpadA";
               break;
            case FS_UP_2:
            case FS_DOWN_2:
            case FS_LEFT_2:
            case FS_RIGHT_2:
               _loc1_ = "UIMenuQuickUseFocusDpadB";
               break;
            case FS_UP_3:
            case FS_DOWN_3:
            case FS_LEFT_3:
            case FS_RIGHT_3:
               _loc1_ = "UIMenuQuickUseFocusDpadC";
         }
         return _loc1_;
      }
      
      public function GetEntryClip(param1:uint) : FavoritesEntry
      {
         var _loc2_:FavoritesEntry = getChildByName("Entry_" + param1) as FavoritesEntry;
         if(_loc2_ == null)
         {
            GlobalFunc.TraceWarning("Could not find the entry \'Entry_" + param1 + "\'!");
         }
         return _loc2_;
      }
      
      // Controller page switching is not implemented here. "LShoulder" and
      // "RShoulder" exist only in Starfield's menu-navigation input contexts,
      // and this wheel resolves its input in the gameplay context, where
      // "Quickkeys", "Quickkey1"-"Quickkey12" and the D-pad "Quickkey*"
      // events live and LB/RB are bound to Monocle and AltAttack instead.
      // Cases for those two names were therefore never reachable. The DLL
      // reads the shoulder buttons itself while the wheel is open.
      public function ProcessUserEvent(param1:String, param2:Boolean) : Boolean
      {
         var _loc4_:Number = NaN;
         var _loc3_:Boolean = false;
         if(!param2)
         {
            _loc3_ = true;
            switch(param1)
            {
               case "Cancel":
               case "Quickkeys":
               case "YButton":
                  this.StartClosingMenu();
                  _loc3_ = true;
                  break;
               default:
                  _loc4_ = Number(param1.substr(8));
                  if(_loc4_ >= 1 && _loc4_ <= FS_NONE)
                  {
                     this.selectedIndex = _loc4_ - 1;
                     this.SelectItem();
                  }
                  else
                  {
                     _loc3_ = false;
                  }
            }
         }
         return _loc3_;
      }
      
      private function StartClosingMenu() : void
      {
         gotoAndPlay("Close");
         addEventListener(this.TIMELINE_EVENT_CLOSE_ANIM_DONE,this.onCloseAnimFinished);
      }
      
      private function onCloseAnimFinished() : void
      {
         removeEventListener(this.TIMELINE_EVENT_CLOSE_ANIM_DONE,this.onCloseAnimFinished);
         GlobalFunc.CloseMenu("FavoritesMenu");
      }
      
      private function onSelectionChange() : void
      {
         if(this.selectedEntry != null)
         {
            this.ItemInfo_mc.UpdateDisplay(this.selectedEntry);
            this.ItemInfo_mc.visible = true;
         }
         else
         {
            this.ItemInfo_mc.visible = false;
         }
      }
      
      public function onKeyDownHandler(param1:KeyboardEvent) : *
      {
         // Starfield has no way to empty a favorite slot: its only
         // slot-clearing routine runs inside the assign routine, to make room
         // before a write. This is the mod's own removal.
         if(this.FavoritesBanksClearSlotKey != 0
            && param1.keyCode == this.FavoritesBanksClearSlotKey
            && this.FavoritesBanksClearSlot != null
            && this.selectedIndex != FS_NONE)
         {
            this.FavoritesBanksClearSlot(this.FavoritesBanksActiveBank * FS_NONE + this.selectedIndex);
            param1.stopPropagation();
            return;
         }
         if(this.FavoritesBanksSwitchRow != null)
         {
            switch(param1.keyCode)
            {
               case Keyboard.UP:
                  this.FavoritesBanksSwitchRow(-1);
                  break;
               case Keyboard.DOWN:
                  this.FavoritesBanksSwitchRow(1);
                  break;
               case Keyboard.LEFT:
                  this.selectedIndex = this.FavoritesBanksStepSlot(-1);
                  break;
               case Keyboard.RIGHT:
                  this.selectedIndex = this.FavoritesBanksStepSlot(1);
            }
            return;
         }
         switch(param1.keyCode)
         {
            case Keyboard.UP:
               this.selectedIndex = this._UpDirectory[this.selectedIndex];
               break;
            case Keyboard.DOWN:
               this.selectedIndex = this._DownDirectory[this.selectedIndex];
               break;
            case Keyboard.LEFT:
               this.selectedIndex = this._LeftDirectory[this.selectedIndex];
               break;
            case Keyboard.RIGHT:
               this.selectedIndex = this._RightDirectory[this.selectedIndex];
         }
      }
      
      // One step along the row the grid draws, clamped at both ends.
      // The vanilla tables jump between the wheel's compass directions,
      // which has nothing to do with a row of twelve cells. selectedIndex
      // is a uint, so the step is computed as an int first: decrementing
      // zero would otherwise wrap to four billion.
      public function FavoritesBanksStepSlot(param1:int) : uint
      {
         var _loc2_:int = int(this.selectedIndex);
         if(_loc2_ < 0 || _loc2_ >= FS_NONE)
         {
            return 0;
         }
         _loc2_ += param1;
         if(_loc2_ < 0)
         {
            _loc2_ = 0;
         }
         if(_loc2_ > int(FS_NONE) - 1)
         {
            _loc2_ = int(FS_NONE) - 1;
         }
         return uint(_loc2_);
      }
      
      public function onKeyUpHandler(param1:KeyboardEvent) : *
      {
         switch(param1.keyCode)
         {
            case Keyboard.ENTER:
               if(this.selectedIndex != FS_NONE)
               {
                  this.SelectItem();
                  param1.stopPropagation();
               }
         }
      }
      
      private function SelectItem() : void
      {
         if(this.selectedIndex == FS_NONE)
         {
            return;
         }
         var _loc1_:uint = uint(this.FavoritesBanksActiveBank * FS_NONE + this.selectedIndex);
         var _loc2_:Boolean = true;
         if(this.FavoritesBanksNativeSelection != null)
         {
            _loc2_ = this.FavoritesBanksNativeSelection(_loc1_,this.isAssigningItem(),this.AssignedItem) == true;
         }
         if(_loc2_)
         {
            if(this.isAssigningItem())
            {
               BSUIDataManager.dispatchEvent(new CustomEvent("FavoritesMenu_AssignQuickkey",{"uQuickkeyIndex":this.selectedIndex}));
            }
            else
            {
               BSUIDataManager.dispatchEvent(new CustomEvent("FavoritesMenu_UseQuickkey",{"uQuickkeyIndex":this.selectedIndex}));
            }
         }
         this.StartClosingMenu();
      }
      
      private function onFavEntryMouseover(param1:Event) : void
      {
         this.selectedIndex = param1.target.entryIndex;
         this.OverEntry = true;
      }
      
      private function onFavEntryMouseleave(param1:Event) : void
      {
         this.OverEntry = false;
      }
      
      internal function frame13() : *
      {
         stop();
      }
      
      internal function frame18() : *
      {
         dispatchEvent(new Event("onFinishedClosingAnim",true,true));
         stop();
      }
      
      internal function frame21() : *
      {
         dispatchEvent(new Event("onFinishedClosingAnim",true,true));
         stop();
      }
   }
}

