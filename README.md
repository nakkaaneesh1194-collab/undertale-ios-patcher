# undertale-ios-patcher

A tool that ports Undertale to iOS.

You need to own Undertale on Steam. The script takes your Steam install and makes an .ipa file from it. No piracy here.

---

## How it works

- **Butterscotch** is the runner used to run the game
- **UndertaleModCli** is used to apply bugs fixes and patches 

---

## Requirementsis 

- **Mac** 
- **XCode** (plus command line tools!)
- **cmake** 
- **SDL2**

---

## Usage

```bash
# 1. Clone this repo
git clone https://github.com/nakkaaneesh1194-collab/undertale-ios-patcher
cd undertale-ios-patcher

# 2. Run the patcher
chmod +x patch_ios.sh
./patch_ios.sh
```

The script will ask you a couple of questions. If unsure, press y.



---

## Installing the IPA

After you get the ipa, you can install it using one of these following tools. I personaly recomend LiveContainer and Sidestore, but others tools should work well.

- **[AltStore](https://altstore.io)** / **[SideStore](https://sidestore.io)** - (recommended)
- **[Sideloadly](https://sideloadly.io)** 
- **[LiveContainer](https://github.com/khanhduytran0/LiveContainer)** — (highly recommended)

---



## Credits

- [Butterscotch](https://github.com/PerfectDreams/Butterscotch) — the runner
- [UndertaleModTool](https://github.com/UnderminersTeam/UndertaleModTool) — used for applying patches

---

## License

Mozilla Public License 2.0   
Undertale is © by Toby Fox. This project does not distribute any game assets.
