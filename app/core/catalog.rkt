#lang racket/base

(require racket/list)

;; Curated catalog of English podcasts, shipped inside the app.
;;
;; Design rule: the catalog is a *discovery aid*, never auto-subscribed —
;; the host shows it and the user adds entries with one tap through the
;; normal feed-add path. Every entry must pass scripts/verify-catalog.rkt
;; (live fetch + parse through the app's own stack) before it ships in a
;; release; entries are removed when they die.
;;
;; Fields: id, category (tech/security/science/design/business/news),
;; name, description (zh/en), url, homepage, artwork-url (Apple CDN,
;; verified reachable at entry time — the hosts render it on cards).

(provide catalog-entries
         catalog-entry-id
         catalog-entry-category
         catalog-entry-name
         catalog-entry-description
         catalog-entry-url
         catalog-entry-homepage
         catalog-entry-artwork
         catalog-categories)

(define entries
  (list
   ;; ---- tech ----------------------------------------------------------
   (hasheq 'id "lex"
           'category "tech"
           'name "Lex Fridman Podcast"
           'description-zh "长对话：AI、科学、哲学、历史，嘉宾从李景风到 Yann LeCun。单集常超两小时，适合配着翻译慢慢啃。"
           'description-en "Long-form conversations on AI, science, philosophy and history — often 2+ hours."
           'url "https://lexfridman.com/feed/podcast/"
           'homepage "https://lexfridman.com/podcast/"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts115/v4/3e/e3/9c/3ee39c89-de08-47a6-7f3d-3849cef6d255/mza_16657851278549137484.png/600x600bb.jpg")
   (hasheq 'id "sedaily"
           'category "tech"
           'name "Software Engineering Daily"
           'description-zh "每日一集的软件工程访谈：数据库、云、区块链、ML 基建，覆盖面极广。"
           'description-en "Daily technical interviews spanning databases, cloud, ML infrastructure and more."
           'url "https://softwareengineeringdaily.com/feed/podcast/"
           'homepage "https://softwareengineeringdaily.com"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts221/v4/36/a1/dd/36a1dd7e-52ed-78f7-e13f-4261a65cfc58/mza_12039623462681143710.png/600x600bb.jpg")
   (hasheq 'id "atp"
           'category "tech"
           'name "Accidental Tech Podcast"
           'description-zh "三位程序员的周更闲谈：Apple、编程语言与行业八卦，程序员圈的经典下饭播客。"
           'description-en "Three programmers talk Apple, programming and the industry weekly."
           'url "https://atp.fm/rss"
           'homepage "https://atp.fm"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts126/v4/91/22/42/9122426f-df98-4302-6ba3-da67b0648e70/mza_5041274938111919910.png/600x600bb.jpg")
   (hasheq 'id "talkshow"
           'category "tech"
           'name "The Talk Show With John Gruber"
           'description-zh "Daring Fireball 博主的招牌节目，Apple 生态与科技评论的第一现场。"
           'description-en "Daring Fireball's John Gruber on Apple and the tech industry."
           'url "https://daringfireball.net/thetalkshow/rss"
           'homepage "https://daringfireball.net/thetalkshow/"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts126/v4/3c/d5/d3/3cd5d37f-bc05-ec52-e700-08bb37839faa/mza_17217490491625030645.png/600x600bb.jpg")
   (hasheq 'id "corecursive"
           'category "tech"
           'name "CoRecursive: Coding Stories"
           'description-zh "讲代码背后的故事：一个 bug 如何搞垮整条产品线、某门语言为何长成今天这样。叙事强，语速友好。"
           'description-en "The stories behind code: famous bugs, design decisions and language histories."
           'url "https://corecursive.com/feed"
           'homepage "https://corecursive.com"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts114/v4/d1/68/fa/d168faa0-54ad-137d-c936-b728cdb61a06/mza_6770579440872689479.png/600x600bb.jpg")
   (hasheq 'id "lennys"
           'category "tech"
           'name "Lenny's Podcast"
           'description-zh "产品与增长的第一访谈：来自 Airbnb/Stripe 级别操盘手的实战经验。"
           'description-en "Product and growth interviews with operators from Airbnb, Stripe and beyond."
           'url "https://www.lennysnewsletter.com/feed"
           'homepage "https://www.lennysnewsletter.com"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts211/v4/c7/80/a3/c780a365-a1ed-527f-365f-1bd3e51ac6d1/mza_5908526734339724450.jpg/600x600bb.jpg")
   (hasheq 'id "acquired"
           'category "tech"
           'name "Acquired"
           'description-zh "把一家公司的兴衰讲成三小时的纪录片式长篇：Nvidia、Tesla、LVHM……"
           'description-en "Documentary-length stories of how great companies came to be."
           'url "https://feeds.transistor.fm/acquired"
           'homepage "https://www.acquired.fm"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts211/v4/a8/e1/de/a8e1deff-9f88-4e55-a541-b0dc793c0cdc/mza_11539673419613154037.jpg/600x600bb.jpg")

   ;; ---- security --------------------------------------------------------
   (hasheq 'id "darknet"
           'category "security"
           'name "Darknet Diaries"
           'description-zh "真实网络攻防故事：社工、APT、暗网执法行动，制作水准堪比美剧。听安全英语的绝佳教材。"
           'description-en "True stories from the dark side of the Internet — social engineering, APTs and takedowns."
           'url "https://feeds.megaphone.fm/darknetdiaries"
           'homepage "https://darknetdiaries.com"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts122/v4/3a/80/a7/3a80a7db-5620-f77b-9935-016e61cc2fbc/mza_9399859904175514567.jpg/600x600bb.jpg")

   ;; ---- science -----------------------------------------------------------
   (hasheq 'id "huberman"
           'category "science"
           'name "Huberman Lab"
           'description-zh "斯坦福神经科学教授讲睡眠、专注、健康的底层机制，术语密集，翻译刚需。"
           'description-en "A Stanford neuroscientist on sleep, focus and health mechanisms."
           'url "https://feeds.megaphone.fm/hubermanlab"
           'homepage "https://hubermanlab.com"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts221/v4/9a/d3/19/9ad31912-0b5a-a16e-2d7c-9fd074698b9c/mza_8994222203629500925.jpg/600x600bb.jpg")
   (hasheq 'id "radiolab"
           'category "science"
           'name "Radiolab"
           'description-zh "科学叙事的黄金标准：把一个冷知识讲成一本短篇小说。"
           'description-en "Investigative science storytelling — the genre's gold standard."
           'url "https://feeds.simplecast.com/EmVW7VGp"
           'homepage "https://radiolab.org"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts211/v4/2b/b2/4d/2bb24d28-f3bb-916f-6bf3-9e125ba5219b/mza_4476298389845914795.jpg/600x600bb.jpg")
   (hasheq 'id "startalk"
           'category "science"
           'name "StarTalk"
           'description-zh "Neil deGrasse Tyson 主持的天体物理与宇宙学，嘉宾含大量一线科学家。"
           'description-en "Neil deGrasse Tyson on astrophysics and cosmology, with working scientists."
           'url "https://feeds.simplecast.com/4T39_jAj"
           'homepage "https://www.startalkradio.net"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts211/v4/d7/88/9b/d7889bab-dca5-77ba-3d0c-7fae8f16ab11/mza_8810454848871508.jpg/600x600bb.jpg")

   ;; ---- design ----------------------------------------------------------
   (hasheq 'id "99pi"
           'category "design"
           'name "99% Invisible"
           'description-zh "设计与建筑如何塑造城市与日常——每集二十分钟，英文播客入门首选之一。"
           'description-en "The design thinking behind cities, objects and everyday life."
           'url "https://feeds.simplecast.com/BqbsxVfO"
           'homepage "https://99percentinvisible.org"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts211/v4/79/d0/35/79d035ea-9043-b43e-7380-33cd47bd968b/mza_2606971010425550919.jpg/600x600bb.jpg")

   ;; ---- business ----------------------------------------------------------
   (hasheq 'id "planetmoney"
           'category "business"
           'name "Planet Money"
           'description-zh "NPR 经济学节目：用二十分钟讲清楚一个经济机制，例子里全是生活。"
           'description-en "NPR's economy show — one mechanism per episode, told through daily life."
           'url "https://feeds.npr.org/510289/podcast.xml"
           'homepage "https://www.npr.org/planetmoney"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts211/v4/85/df/53/85df5334-0fae-28a9-2bc4-b97b81061d0e/mza_10839245066228881011.jpg/600x600bb.jpg")
   (hasheq 'id "hibt"
           'category "business"
           'name "How I Built This"
           'description-zh "创始人访谈：Airbnb、Patagonia……从第一性原理听创业叙事。"
           'description-en "Founders tell the stories behind the companies they built."
           'url "https://rss.art19.com/how-i-built-this"
           'homepage "https://www.npr.org/howibuiltthis"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts126/v4/64/45/06/644506b5-c44f-f661-f74e-f63a4b2511bc/mza_14892199991035639268.jpeg/600x600bb.jpg")

   ;; ---- news / ideas ------------------------------------------------------
   (hasheq 'id "thedaily"
           'category "news"
           'name "The Daily"
           'description-zh "纽约时报每日新闻深读，二十分钟理解当天最重要的一件事。"
           'description-en "The New York Times' daily news show — one big story in twenty minutes."
           'url "https://feeds.simplecast.com/54nAGcIl"
           'homepage "https://www.nytimes.com/thedaily"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts221/v4/ab/64/66/ab6466a9-9a7d-e20e-7a3d-bc5be37d29ce/mza_15084852813176276273.jpg/600x600bb.jpg")
   (hasheq 'id "ted"
           'category "news"
           'name "TED Talks Daily"
           'description-zh "每天一场 TED 演讲音频版，话题横跨科技/人文/气候。"
           'description-en "A TED talk every weekday, from technology to climate to culture."
           'url "https://feeds.acast.com/public/shows/67587e77c705e441797aff96"
           'homepage "https://www.ted.com/podcasts/ted-talks-daily"
           'artwork-url "https://is1-ssl.mzstatic.com/image/thumb/Podcasts116/v4/2e/cf/99/2ecf996f-71f7-604f-b0a0-43116b9d6619/mza_10257768296573848480.png/600x600bb.jpg")))

(define (catalog-entries) entries)
(define (catalog-entry-id e) (hash-ref e 'id))
(define (catalog-entry-category e) (hash-ref e 'category))
(define (catalog-entry-name e) (hash-ref e 'name))
(define (catalog-entry-description e [lang "zh"])
  (hash-ref e (if (equal? lang "en") 'description-en 'description-zh) ""))
(define (catalog-entry-url e) (hash-ref e 'url))
(define (catalog-entry-homepage e) (hash-ref e 'homepage))
(define (catalog-entry-artwork e) (hash-ref e 'artwork-url ""))

(define (catalog-categories)
  (sort (remove-duplicates (map catalog-entry-category entries)) string<?))
