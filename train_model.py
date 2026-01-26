import pandas as pd
import numpy as np
import os

from sklearn.preprocessing import LabelEncoder
from sklearn.model_selection import train_test_split, cross_val_score
from sklearn.ensemble import RandomForestClassifier
from xgboost import XGBClassifier
from sklearn.metrics import (accuracy_score, classification_report, confusion_matrix, precision_score, recall_score, f1_score)
from sklearn.impute import SimpleImputer
import pickle
import warnings

warnings.filterwarnings("ignore", category=UserWarning)

print("="*70)
print("  IoT Urine Analyzer - Model Training Script ".center(70))
print("="*70)

# Load data dari Google Sheets
print("\n Loading dataset from Google Sheets...")
url = "https://docs.google.com/spreadsheets/d/1R6ehqTzOacp_BCm_XXuvCa180WXsC6x9YLdjg2PuaWU/export?format=csv"
df = pd.read_csv(url)

print(f" Dataset loaded successfully!")
print(f" Rows: {df.shape[0]}")
print(f" Columns: {df.shape[1]}")
print(f" Columns: {df.columns.tolist()}")

print("\n Sample Data:")
print(df.head())

# Preprocessing
print("\n Preprocessing data...")
df['Berat Jenis'] = df['Berat Jenis'].astype(str).str.replace(',', '.').astype(float)

# Generate NTU jika belum ada
if 'NTU' not in df.columns:
    print("  Dataset doesn't have NTU column. Generating from Kejernihan...")
    
    def estimate_ntu_from_kejernihan(kejernihan):
        kejernihan_str = str(kejernihan).upper()
        if 'JERNIH' in kejernihan_str or 'BENING' in kejernihan_str:
            return np.random.uniform(0, 10)  # Jernih: < 10 NTU
        elif 'SEDIKIT' in kejernihan_str or 'AGAK' in kejernihan_str:
            return np.random.uniform(10, 30)  # Agak Keruh: 10-30 NTU
        elif 'KERUH' in kejernihan_str:
            return np.random.uniform(30, 60)  # Keruh: >= 30 NTU
        else:
            return np.random.uniform(5, 15)
    
    df['NTU'] = df['Kejernihan'].apply(estimate_ntu_from_kejernihan)
    print(" NTU column generated successfully")

# Features and target
X = df[['Warna', 'Kejernihan', 'pH', 'Berat Jenis', 'NTU']].copy()
y = df['Data Penyakit'].copy()

print(f"\n Features used: {X.columns.tolist()}")
print(f" Total samples: {len(X)}")
print(f" Target classes: {y.unique().tolist()}")

# Label encoding
print("\n  Encoding categorical variables...")
le_warna = LabelEncoder()
le_kejernihan = LabelEncoder()
le_penyakit = LabelEncoder()

X['Warna'] = le_warna.fit_transform(X['Warna'].astype(str))
X['Kejernihan'] = le_kejernihan.fit_transform(X['Kejernihan'].astype(str))
y = le_penyakit.fit_transform(y.astype(str))

print(f"  Warna classes: {le_warna.classes_.tolist()}")
print(f"  Kejernihan classes: {le_kejernihan.classes_.tolist()}")
print(f"  Disease classes: {le_penyakit.classes_.tolist()}")

# Imputation for missing values
print("\n Handling missing values...")
imputer_num = SimpleImputer(strategy="mean")
X[['pH', 'Berat Jenis', 'NTU']] = imputer_num.fit_transform(X[['pH', 'Berat Jenis', 'NTU']])

# Train-test split
print("\n  Splitting data (80% train, 20% test)...")
X_train, X_test, y_train, y_test = train_test_split(
    X, y, test_size=0.2, random_state=42, stratify=y
)
print(f"   ├─ Training samples: {len(X_train)}")
print(f"   └─ Testing samples: {len(X_test)}")

# Training Random Forest
print("\n" + "="*70)
print("  TRAINING RANDOM FOREST ".center(70))
print("="*70)
rf = RandomForestClassifier(n_estimators=100, random_state=42, max_depth=10)
rf.fit(X_train, y_train)
print(" Random Forest trained successfully!")

# Training XGBoost
print("\n" + "="*70)
print("  TRAINING XGBOOST ".center(70))
print("="*70)
xgb = XGBClassifier(use_label_encoder=False, eval_metric='mlogloss', random_state=42, max_depth=6)
xgb.fit(X_train, y_train)
print(" XGBoost trained successfully!")

# Feature Importance
print("\n" + "="*70)
print("  FEATURE IMPORTANCE (Random Forest) ".center(70))
print("="*70)
feature_names = ['Warna', 'Kejernihan', 'pH', 'Berat Jenis', 'NTU']
importances = rf.feature_importances_
for name, importance in sorted(zip(feature_names, importances), key=lambda x: x[1], reverse=True):
    bar = '█' * int(importance * 50)
    print(f"{name:<15}: {importance:.4f} {bar}")

# Model Evaluation
print("\n" + "="*70)
print(" MODEL EVALUATION ".center(70))
print("="*70)

rf_pred = rf.predict(X_test)
xgb_pred = xgb.predict(X_test)

# Metrics untuk Random Forest
rf_accuracy = accuracy_score(y_test, rf_pred)
rf_precision = precision_score(y_test, rf_pred, average='weighted', zero_division=0)
rf_recall = recall_score(y_test, rf_pred, average='weighted', zero_division=0)
rf_f1 = f1_score(y_test, rf_pred, average='weighted', zero_division=0)

# Metrics untuk XGBoost
xgb_accuracy = accuracy_score(y_test, xgb_pred)
xgb_precision = precision_score(y_test, xgb_pred, average='weighted', zero_division=0)
xgb_recall = recall_score(y_test, xgb_pred, average='weighted', zero_division=0)
xgb_f1 = f1_score(y_test, xgb_pred, average='weighted', zero_division=0)

# Cross-validation
print("\n Cross-Validation (5-fold):")
rf_cv_scores = cross_val_score(rf, X_train, y_train, cv=5, scoring='f1_weighted')
xgb_cv_scores = cross_val_score(xgb, X_train, y_train, cv=5, scoring='f1_weighted')

print(f"  Random Forest CV F1: {rf_cv_scores.mean():.3f} ± {rf_cv_scores.std():.3f}")
print(f"  XGBoost CV F1: {xgb_cv_scores.mean():.3f} ± {xgb_cv_scores.std():.3f}")

# Performance Metrics Table
print("\n Performance Metrics on Test Set:")
print("="*70)
print(f"{'Metric':<15} {'Random Forest':<20} {'XGBoost':<20}")
print("="*70)
print(f"{'Accuracy':<15} {rf_accuracy:<20.3f} {xgb_accuracy:<20.3f}")
print(f"{'Precision':<15} {rf_precision:<20.3f} {xgb_precision:<20.3f}")
print(f"{'Recall':<15} {rf_recall:<20.3f} {xgb_recall:<20.3f}")
print(f"{'F1-Score':<15} {rf_f1:<20.3f} {xgb_f1:<20.3f}")
print("="*70)

# Best Model
best_model = 'XGBoost' if xgb_f1 > rf_f1 else 'Random Forest'
print(f"\n Best Model: {best_model}")

# Classification Report
print("\n" + "="*70)
print("  CLASSIFICATION REPORT - RANDOM FOREST ".center(70))
print("="*70)
print(classification_report(y_test, rf_pred, target_names=le_penyakit.classes_, zero_division=0))

print("\n" + "="*70)
print("  CLASSIFICATION REPORT - XGBOOST ".center(70))
print("="*70)
print(classification_report(y_test, xgb_pred, target_names=le_penyakit.classes_, zero_division=0))

# Confusion Matrix
print("\n" + "="*70)
print("  CONFUSION MATRIX ".center(70))
print("="*70)

print("\nRandom Forest:")
rf_cm = confusion_matrix(y_test, rf_pred)
print("\n" + " " * 15 + "Predicted →")
print(" " * 10 + " ".join([f"{cls[:8]:<10}" for cls in le_penyakit.classes_]))
print("Actual ↓")
for i, actual_class in enumerate(le_penyakit.classes_):
    row_str = f"{actual_class[:8]:<10}"
    for val in rf_cm[i]:
        row_str += f"{val:<10}"
    print(row_str)

print("\nXGBoost:")
xgb_cm = confusion_matrix(y_test, xgb_pred)
print("\n" + " " * 15 + "Predicted →")
print(" " * 10 + " ".join([f"{cls[:8]:<10}" for cls in le_penyakit.classes_]))
print("Actual ↓")
for i, actual_class in enumerate(le_penyakit.classes_):
    row_str = f"{actual_class[:8]:<10}"
    for val in xgb_cm[i]:
        row_str += f"{val:<10}"
    print(row_str)

# Save models
print("\n" + "="*70)
print("  SAVING MODELS ".center(70))
print("="*70)

models = {
    'rf': rf,
    'xgb': xgb,
    'le_warna': le_warna,
    'le_kejernihan': le_kejernihan,
    'le_penyakit': le_penyakit,
    'imputer': imputer_num,
    'rf_f1': rf_f1,
    'xgb_f1': xgb_f1
}

model_filename = 'trained_models.pkl'
with open(model_filename, 'wb') as f:
    pickle.dump(models, f)

print(f" Models saved to '{model_filename}'")
print(f" Random Forest F1: {rf_f1:.3f}")
print(f" XGBoost F1: {xgb_f1:.3f}")
print(f" File size: {os.path.getsize(model_filename) / 1024:.2f} KB")

# Test prediction with MongoDB data (optional)
print("\n" + "="*70)
print("  TESTING WITH MONGODB DATA ".center(70))
print("="*70)

try:
    from pymongo import MongoClient
    
    client = MongoClient("mongodb://localhost:27017/", serverSelectionTimeoutMS=5000)
    db = client["iot_urine"]
    collection = db["sensordataaaa"]
    
    mongo_data = collection.find_one(sort=[("_id", -1)])
    
    if mongo_data:
        print("\n Latest data from MongoDB found!")
        print(f"  Timestamp: {mongo_data.get('timestamp', 'N/A')}")
        print(f"  pH: {mongo_data.get('ph', 'N/A')}")
        print(f"  TDS: {mongo_data.get('tds', 'N/A')}")
        print(f"  Turbidity: {mongo_data.get('turbidityLevel', 'N/A')}")
        print(f"  NTU: {mongo_data.get('turbidityNTU', 'N/A')}")
        
        # Preprocess test data
        def tds_to_sg(tds):
            if tds < 50:
                return 1.005
            elif tds <= 500:
                return round(1.005 + (tds - 50) * 0.000011, 3)
            elif tds <= 1500:
                return round(1.010 + (tds - 500) * 0.000015, 3)
            else:
                return round(1.025 + (tds - 1500) * 0.000003, 3)
        
        ph = float(mongo_data.get('ph', 7.0))
        
        warna = mongo_data.get('warnaDasar', '').title()
        if not warna or warna == 'N/A':
            warna = 'Kuning'
        
        turbidity_level = mongo_data.get('turbidityLevel', '').upper()
        if 'JERNIH' in turbidity_level:
            kejernihan = 'Jernih'
        elif 'AGAK' in turbidity_level:
            kejernihan = 'Agak Keruh'
        elif 'KERUH' in turbidity_level:
            kejernihan = 'Keruh'
        else:
            kejernihan = 'Jernih'
        
        ntu = float(mongo_data.get('turbidityNTU', 5))
        
        sg = float(mongo_data.get('specificGravity', 0))
        if not sg:
            sg = tds_to_sg(float(mongo_data.get('tds', 0)))
        
        # Encode
        try:
            warna_enc = le_warna.transform([warna])[0]
        except:
            warna_enc = le_warna.transform(['Kuning'])[0]
        
        try:
            kejernihan_enc = le_kejernihan.transform([kejernihan])[0]
        except:
            kejernihan_enc = le_kejernihan.transform(['Jernih'])[0]
        
        X_test_mongo = [[warna_enc, kejernihan_enc, ph, sg, ntu]]
        
        # Predict
        rf_pred_mongo = rf.predict(X_test_mongo)[0]
        rf_proba_mongo = rf.predict_proba(X_test_mongo)[0]
        rf_label_mongo = le_penyakit.inverse_transform([rf_pred_mongo])[0]
        rf_conf_mongo = np.max(rf_proba_mongo)
        
        xgb_pred_mongo = xgb.predict(X_test_mongo)[0]
        xgb_proba_mongo = xgb.predict_proba(X_test_mongo)[0]
        xgb_label_mongo = le_penyakit.inverse_transform([xgb_pred_mongo])[0]
        xgb_conf_mongo = np.max(xgb_proba_mongo)
        
        # Ensemble
        weighted_proba = (rf_f1 * rf_proba_mongo + xgb_f1 * xgb_proba_mongo) / (rf_f1 + xgb_f1)
        ensemble_pred = np.argmax(weighted_proba)
        ensemble_label = le_penyakit.inverse_transform([ensemble_pred])[0]
        ensemble_conf = np.max(weighted_proba)
        
        print("\n Test Prediction Results:")
        print(f"  Random Forest: {rf_label_mongo} ({rf_conf_mongo:.1%})")
        print(f"  XGBoost: {xgb_label_mongo} ({xgb_conf_mongo:.1%})")
        print(f"  Ensemble: {ensemble_label} ({ensemble_conf:.1%})")
        
    else:
        print("\n No data found in MongoDB")
        
except Exception as e:
    print(f"\n Cannot connect to MongoDB: {e}")
    print("   Skipping MongoDB test...")

# Summary
print("\n" + "="*70)
print(" TRAINING COMPLETE ".center(70))
print("="*70)
print(f"""
Summary:
    Dataset: {len(X)} samples
    Features: {len(feature_names)} ({', '.join(feature_names)})
    Classes: {len(le_penyakit.classes_)} ({', '.join(le_penyakit.classes_)})
    Best Model: {best_model}
    Best F1-Score: {max(rf_f1, xgb_f1):.3f}
    Model saved: {model_filename}

Next Steps:
   1. Run backend server: node server.js
   2. Backend will automatically call ml_predict.py for new data
   3. Open frontend to see real-time predictions
   4. Check ML History tab for prediction results

 Note: Make sure MongoDB is running before testing predictions!
""")
print("="*70 + "\n")