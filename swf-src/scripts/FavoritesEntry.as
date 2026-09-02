package
{
   import Components.ImageFixture;
   import Shared.AS3.BSDisplayObject;
   import Shared.GlobalFunc;
   import Shared.PlatformUtils;
   import flash.display.MovieClip;
   import flash.events.Event;
   import flash.events.MouseEvent;
   import flash.text.TextField;
   import scaleform.gfx.Extensions;
   import scaleform.gfx.TextFieldEx;
   
   public class FavoritesEntry extends BSDisplayObject
   {
      
      public static const MOUSE_OVER:String = "FavoritesEntry::mouse_over";
      
      public static const MOUSE_LEAVE:String = "FavoritesEntry::mouse_leave";
      
      public static const CLICK:String = "FavoritesEntry::mouse_click";
      
      public var Icon_mc:ImageFixture;
      
      public var Quickkey_tf:TextField;
      
      public var SlotInfoSpacer_mc:MovieClip;
      
      public var Catcher_mc:MovieClip;
      
      private var _EntryIndex:uint;

      private var OrigTextColor:uint;

      private var _LoadedKey:String = "<none>";

      private var _PendingItem:Object = null;

      // Set whenever _PendingItem is staged, including when it is staged
      // as null. Testing _PendingItem for null instead meant a slot that
      // went from occupied to empty never reached ApplyIcon at all, so it
      // kept showing the icon of the item that used to be there.
      private var _HasPendingItem:Boolean = false;

      private var _PendingLoad:Boolean = false;

      private var _CurrentItem:Object = null;

      private var _AppliedBound:Number = -1;

      private var _FramesSinceLoad:int = 0;
      
      public function FavoritesEntry()
      {
         super();
         addEventListener(MouseEvent.MOUSE_OVER,this.onMouseOver);
         addEventListener(MouseEvent.MOUSE_OUT,this.onMouseLeave);
         addEventListener(MouseEvent.CLICK,this.onMousePress);
         this._EntryIndex = uint(this.name.substr(this.name.lastIndexOf("_") + 1));
         this.OrigTextColor = this.Quickkey_tf.textColor;
         Extensions.enabled = true;
         TextFieldEx.setTextAutoSize(this.Quickkey_tf,TextFieldEx.TEXTAUTOSZ_SHRINK);
         if(this.Catcher_mc)
         {
            this.hitArea = this.Catcher_mc;
            this.Catcher_mc.mouseEnabled = false;
         }
      }
      
      override protected function OnControlMapChanged(param1:Object) : void
      {
         var _loc3_:Object = null;
         super.OnControlMapChanged(param1);
         this.Quickkey_tf.visible = this.uiController == PlatformUtils.PLATFORM_PC_KB_MOUSE;
         var _loc2_:String = "Quickkey" + (this._EntryIndex + 1);
         for each(_loc3_ in param1.vMappedEvents)
         {
            if(_loc3_.strUserEventName == _loc2_)
            {
               GlobalFunc.SetText(this.Quickkey_tf,_loc3_.strButtonName,false);
               break;
            }
         }
      }
      
      public function get entryIndex() : uint
      {
         return this._EntryIndex;
      }
      
      public function set selected(param1:Boolean) : void
      {
         gotoAndStop(param1 ? "Selected" : "Unselected");
         this.Quickkey_tf.textColor = param1 ? 0 : this.OrigTextColor;
      }
      
      private function DescribeIcon(param1:Object) : String
      {
         if(param1 == null || param1.iconImage == null)
         {
            return "";
         }
         return param1.iconImage.iFixtureType + "|" + param1.iconImage.sDirectory + "|" + param1.iconImage.sImageName + "|" + (param1.bIsPower == true ? "1" : "0");
      }

      // param2 forces the icon to be attached again even when it describes
      // the same image. Skipping identical keys is what stops a page change
      // from restarting every icon's asynchronous load, and it must stay for
      // repeated renders of the same page. It is wrong across a page change
      // though: if a symbol lost its attach race the entry still holds the
      // key of an icon that is not on screen, and nothing short of closing
      // and reopening the wheel would ever ask for it again. That is exactly
      // the "page is empty until you reopen it" report.
      public function LoadIcon(param1:Object, param2:Boolean = false) : void
      {
         var _loc3_:String = this.DescribeIcon(param1);
         if(!param2 && _loc3_ == this._LoadedKey)
         {
            return;
         }
         this._LoadedKey = _loc3_;
         this._PendingItem = param1;
         this._HasPendingItem = true;
         if(!this._PendingLoad)
         {
            this._PendingLoad = true;
            addEventListener(Event.ENTER_FRAME,this.onIconLoadFrame);
         }
      }

      private function onIconLoadFrame(param1:Event) : void
      {
         if(this._HasPendingItem)
         {
            this._CurrentItem = this._PendingItem;
            this._PendingItem = null;
            this._HasPendingItem = false;
            this._FramesSinceLoad = 0;
            this.ApplyIcon(this._CurrentItem);
            return;
         }
         // Reloading on a bound change was wrong: every observed change was
         // 80 -> 100, and forcing the reload made the engine size every icon
         // against 100, which is the oversized result. Only measure here.
         this._FramesSinceLoad++;
         if(this._FramesSinceLoad == 1 || this._FramesSinceLoad == 10 || this._FramesSinceLoad == 30 || this._FramesSinceLoad == 90)
         {
            var _loc2_:Object = this.Icon_mc.BoundClip_mc;
            var _loc3_:Object = this.Icon_mc.symbolInstance;
            this.Report("+" + this._FramesSinceLoad + "f preload=" + this._AppliedBound
               + " bound=" + (_loc2_ == null ? "NULL" : String(_loc2_.width))
               + " scale=" + (_loc3_ == null ? "-" : String(_loc3_.scaleX))
               + " onscreen=" + (_loc3_ == null ? "-" : String(_loc3_.width)));
         }
         if(this._FramesSinceLoad > 90)
         {
            removeEventListener(Event.ENTER_FRAME,this.onIconLoadFrame);
            this._PendingLoad = false;
         }
      }

      private function Report(param1:String) : void
      {
         var _loc2_:Object = this.parent;
         if(_loc2_ != null && _loc2_.FavoritesBanksLog != null)
         {
            _loc2_.FavoritesBanksLog("entry " + this._EntryIndex + " " + param1);
         }
      }

      private function ApplyIcon(param1:Object) : void
      {
         // ItemIcon is a four frame clip that plays itself open and stops at
         // frame 3, and its BoundClip_mc is scaled 1.2 / 1.6 / 2.0 across
         // those frames, so the bound measures 60, 80 or 100 depending on
         // when it is read. BaseLoaderClip.AddDisplayObject reads it once,
         // at the instant the symbol is attached, and symbols arrive
         // asynchronously. That race is why the same icon came out at a
         // different size on each open. Park the clip on its resting frame
         // first so the bound is always the one the wheel settles on.
         var _loc3_:Object = this.parent;
         var _loc4_:int = 2;
         if(_loc3_ != null && _loc3_.FavoritesBanksIconFrame > 0)
         {
            _loc4_ = int(_loc3_.FavoritesBanksIconFrame);
         }
         this.Icon_mc.gotoAndStop(_loc4_);
         var _loc2_:Object = this.Icon_mc.BoundClip_mc;
         this._AppliedBound = _loc2_ == null ? -1 : _loc2_.width;
         this.Icon_mc.Unload();
         if(param1 == null || param1.iconImage == null || param1.iconImage.iFixtureType == ImageFixture.FT_INVALID)
         {
            this.Icon_mc.clipSizer = "";
            this.Icon_mc.centerClip = false;
            this.Icon_mc.visible = false;
         }
         else
         {
            if(param1.iconImage.iFixtureType == ImageFixture.FT_SYMBOL)
            {
               this.Icon_mc.clipSizer = param1.bIsPower == true ? "Sizer_mc" : "";
               this.Icon_mc.centerClip = true;
            }
            else
            {
               this.Icon_mc.clipSizer = "";
               this.Icon_mc.centerClip = false;
            }
            this.Icon_mc.onLoadAttemptComplete = this.onIconLoadAttemptComplete;
            this.Icon_mc.LoadImageFixtureFromUIData(param1.iconImage,"FavoritesIconBuffer");
            this.Icon_mc.visible = true;
         }
      }

      public function onIconLoadAttemptComplete() : void
      {
         // BaseLoaderClip.AddDisplayObject scales the symbol to fit
         // BoundClip_mc, but only when the loader was given one. Without it
         // the else branch applies ClipScale, which is 1, and the symbol is
         // drawn at its natural size. Report what the engine actually had.
         var _loc1_:Object = this.Icon_mc.symbolInstance;
         var _loc2_:Object = this.Icon_mc.BoundClip_mc;
         this.Report("key=" + this._LoadedKey
            + " bound=" + (_loc2_ == null ? "NULL" : _loc2_.width + "x" + _loc2_.height)
            + " symbol=" + (_loc1_ == null ? "NULL" : _loc1_.width + "x" + _loc1_.height)
            + " scale=" + (_loc1_ == null ? "-" : String(_loc1_.scaleX)));
         this.Icon_mc.mouseEnabled = false;
         this.Icon_mc.mouseChildren = false;
      }
      
      public function onMousePress(param1:MouseEvent) : void
      {
         dispatchEvent(new Event(CLICK,true,true));
      }
      
      public function onMouseOver(param1:MouseEvent) : void
      {
         dispatchEvent(new Event(MOUSE_OVER,true,true));
      }
      
      public function onMouseLeave(param1:MouseEvent) : void
      {
         dispatchEvent(new Event(MOUSE_LEAVE,true,true));
      }
   }
}

